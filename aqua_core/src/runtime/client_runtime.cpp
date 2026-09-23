#include "aqua/runtime/client_runtime.h"
#include "aqua/audio/buffer/buffer_config.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"
#include "aqua/net/net_error.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

// 控制面（push strand / 控制线程）日志开关：默认关闭。开启后本编译单元的
// 决策与判定日志会同步打 spdlog；仅开发期使用。
// 边界：运行在 push strand / 控制线程上的决策与判定；RT 音频回调与 JitterBuffer
// pull() 的日志归 AQUA_JB_RUNTIME_THREAD_DEBUG_LOG。点位全表见
// aqua_core/doc/modules/observability.md。
#ifndef AQUA_JB_CONTROL_THREAD_DEBUG_LOG
#define AQUA_JB_CONTROL_THREAD_DEBUG_LOG 0
#endif

namespace aqua::runtime {

namespace {

    // push strand 事件日志的节流状态（#7 reject / #9 reanchor 共用模式）：
    // 首次即时，之后每秒最多一行；行内给出与上次打印之间的增量（累计计数器
    // 的差分）。只在 arrival observer（push strand）内读写，无并发。
    struct JbRejectLogState {
        std::uint64_t late = 0;
        std::uint64_t busy = 0;
        std::uint64_t invalid = 0;
        std::uint64_t sanity = 0;
        std::int64_t last_log_ns = 0;
        bool logged = false;
    };

    // reanchor 应用事件的节流状态（#9）：每次重锚定都意味着扔掉
    // 一批已缓存音频（可闻断裂），值得一行 warn；风暴期按秒汇总次数，
    // 不逐次刷屏。同样只在 arrival observer 内读写。
    struct ReanchorLogState {
        std::uint64_t count = 0;
        std::int64_t last_log_ns = 0;
        bool logged = false;
    };

    // push strand 事件节流周期（reject #7 门内日志与 reanchor #9 无门日志共用）。
    constexpr std::int64_t kJbRejectLogIntervalNs
        = static_cast<std::int64_t>(config::JB_CONTROL_LOG_REJECT_INTERVAL_MS * 1'000'000.0);

} // namespace

ClientRuntime::ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , device_event_timer_(ioc)
    , grpc_()
    , udp_(ioc)
    , callback_gate_(std::make_shared<CallbackGate>(this))
{
    log_debug("ClientRuntime instance created");
}

ClientRuntime::~ClientRuntime()
{
    stop();
    if (callback_gate_) {
        callback_gate_->detach();
    }
}

bool ClientRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    const bool entered = state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (entered) {
        log_debug("ClientRuntime state: Created -> Starting");
    }
    return entered;
}

bool ClientRuntime::enter_stopping() noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Stopping || state == RuntimeState::Stopped) {
            return false;
        }
        if (state_.compare_exchange_weak(state, RuntimeState::Stopping,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            log_debug("ClientRuntime state: -> Stopping");
            return true;
        }
    }
}

void ClientRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
    log_debug("ClientRuntime state: -> Stopped");
}

bool ClientRuntime::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!enter_starting()) {
        return false;
    }
    if (config_.jb_capacity_slots < config::MIN_JB_CAPACITY_SLOTS
        || config_.jb_capacity_slots > config::MAX_JB_CAPACITY_SLOTS) {
        log_error_fmt("ClientRuntime: jb_capacity_slots must be {}..{}",
            config::MIN_JB_CAPACITY_SLOTS, config::MAX_JB_CAPACITY_SLOTS);
        stop_locked();
        return false;
    }

    if (config_.heartbeat_handshake_interval <= std::chrono::milliseconds(0)) {
        log_error_fmt("ClientRuntime: invalid configuration: heartbeat_handshake_interval={}ms must be > 0",
            config_.heartbeat_handshake_interval.count());
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime config: server={} client_name='{}' jb_capacity={} heartbeat_handshake_interval={}ms playback_device={} playback_buffer_frames={} playback_low_latency={} prefer_current={}",
        aqua::net::format_host_port(config_.server_ip, config_.rpc_port), config_.client_name, config_.jb_capacity_slots,
        config_.heartbeat_handshake_interval.count(),
        config_.playback.device ? config_.playback.device->value() : std::string("default"),
        config_.playback.frames_per_buffer,
        config_.playback.low_latency,
        config_.playback_prefer_current);

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        log_error("ClientRuntime: audio device manager is unavailable on this platform");
        stop_locked();
        return false;
    }
    playback_ = std::make_unique<audio::PlaybackManager>(*device_mgr_);
    if (!playback_->available()) {
        log_error("ClientRuntime: audio playback backend is unavailable on this platform");
        stop_locked();
        return false;
    }

    connect_result_ = { };
    log_info_fmt("ClientRuntime: connecting to gRPC server {}", aqua::net::format_host_port(config_.server_ip, config_.rpc_port));
    if (!grpc_.connect_to_server(config_.server_ip, config_.rpc_port)
        || !grpc_.connect(config_.client_name, connect_result_)
        || !connect_result_.is_valid()) {
        log_error_fmt("ClientRuntime: control-plane connection to {} failed",
            aqua::net::format_host_port(config_.server_ip, config_.rpc_port));
        stop_locked();
        return false;
    }

    if (!connect_result_.audio_format.is_valid()) {
        log_error("ClientRuntime: server returned invalid audio format");
        stop_locked();
        return false;
    }
    if (connect_result_.frame_count == 0) {
        log_error("ClientRuntime: server returned frame_count=0");
        stop_locked();
        return false;
    }
    const auto remote_frame_bytes = connect_result_.audio_format.frame_bytes();
    if (remote_frame_bytes == 0
        || static_cast<std::size_t>(connect_result_.frame_count)
            > std::numeric_limits<std::size_t>::max() / remote_frame_bytes) {
        log_error_fmt("ClientRuntime: remote frame geometry overflows (frame_count={} frame_bytes={})",
            connect_result_.frame_count, remote_frame_bytes);
        stop_locked();
        return false;
    }
    const auto expected_payload_bytes = static_cast<std::size_t>(connect_result_.frame_count) * remote_frame_bytes;
    if (expected_payload_bytes == 0 || expected_payload_bytes > aqua::config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ClientRuntime: received frame payload {} bytes exceeds UDP safe payload budget {} bytes",
            expected_payload_bytes, aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime: validated remote stream geometry: payload_bytes={} jb_capacity={}",
        expected_payload_bytes, config_.jb_capacity_slots);
    // proto keepalive 探活（Connect 成功后立即启动）：控制面死亡或会话失效
    // 即 Degraded（supervision 观察到后 stop + 退出），不重试。
    grpc_.start_keepalive(connect_result_.session_id, config::GRPC_KEEPALIVE_INTERVAL,
        [gate = callback_gate_](grpc::GrpcClient::KeepaliveStatus status) noexcept {
            gate->invoke([status](ClientRuntime& owner) noexcept {
                owner.on_control_plane_dead(status);
            });
        });
    if (!setup_playback(connect_result_.audio_format, connect_result_.frame_count)) {
        log_error("ClientRuntime: failed to create playback/JitterBuffer pipeline");
        stop_locked();
        return false;
    }
    log_debug_fmt("ClientRuntime playback/JitterBuffer pipeline ready: frame_count={} frame_bytes={} jb_slots={}",
        frame_count_, frame_bytes_, config_.jb_capacity_slots);

    const auto effective_udp_port = config_.udp_force_port.value_or(connect_result_.advertised_udp_port);
    if (config_.udp_force_port) {
        log_info_fmt("ClientRuntime: overriding Server-advertised UDP port {} with forced port {}",
            connect_result_.advertised_udp_port, *config_.udp_force_port);
    }
    if (const auto remote = udp_.set_remote(connect_result_.advertised_udp_address, effective_udp_port);
        !remote) {
        log_error_fmt("ClientRuntime: failed to configure UDP remote {}: {}",
            aqua::net::format_host_port(connect_result_.advertised_udp_address, effective_udp_port),
            aqua::net::net_error_name(remote.error()));
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime: starting UDP receive, expected payload={} bytes", expected_payload_bytes);
    if (const auto started = udp_.start_receive(expected_payload_bytes,
            [gate = callback_gate_, jb = jb_, frame_count = frame_count_](
                std::uint64_t sequence, std::span<const std::byte> pcm) {
                const audio::AudioFrame frame { sequence, frame_count, pcm };
                (void)jb->push(frame);
                const auto rejected = jb->take_reanchor_sanity_rejections();
                if (rejected != 0) {
                    gate->invoke([rejected](ClientRuntime& owner) noexcept {
                        owner.on_reanchor_sanity_failure(rejected);
                    });
                }
            });
        !started) {
        log_error_fmt("ClientRuntime: failed to start UDP receive loop: {}",
            aqua::net::net_error_name(started.error()));
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime UDP receive loop started");
    log_debug_fmt("ClientRuntime: starting heartbeat, handshake interval={}ms",
        config_.heartbeat_handshake_interval.count());
    if (!udp_.start_heartbeat(connect_result_.session_id, config_.heartbeat_handshake_interval,
            [gate = callback_gate_](std::uint32_t misses) noexcept {
                gate->invoke([misses](ClientRuntime& owner) noexcept {
                    owner.on_network_liveness_failure(misses);
                });
            })) {
        log_error("ClientRuntime: failed to start heartbeat");
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime heartbeat started");

    auto pb_cfg = config_.playback;
    pb_cfg.format = connect_result_.audio_format;
    // 路由起步（连接属性）：prefer_current = "自动切换播放设备"关。
    playback_->set_prefer_current_on_start(config_.playback_prefer_current);
    auto playback_start = playback_->start(pb_cfg, [this](std::span<std::byte> output) noexcept { return pull_playback(output); }, [this](audio::AudioError error) noexcept { on_playback_event(error); });
    if (!playback_start && pb_cfg.device.has_value()) {
        // 起步指定设备失效（连接间隙被拔 / 格式不兼容）：回退系统默认重试
        // 一次（"永不主动静音"），连接不因此失败；降级经诊断 route_mode 观察。
        log_warn_fmt("ClientRuntime: initial playback device '{}' failed ({}), falling back to system default",
            pb_cfg.device->value(), audio::audio_error_name(playback_start.error()));
        pb_cfg.device.reset();
        playback_start = playback_->start(pb_cfg, [this](std::span<std::byte> output) noexcept { return pull_playback(output); }, [this](audio::AudioError error) noexcept { on_playback_event(error); });
    }
    if (!playback_start) {
        log_error_fmt("ClientRuntime: failed to start audio playback: {}",
            audio::audio_error_name(playback_start.error()));
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime playback backend started");

    RuntimeState expected = RuntimeState::Starting;
    (void)state_.compare_exchange_strong(expected, RuntimeState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);
    const auto final_state = state_.load(std::memory_order_acquire);
    log_debug_fmt("ClientRuntime startup completed with state={}", runtime_state_name(final_state));
    if (final_state == RuntimeState::Running || final_state == RuntimeState::Degraded) {
        log_info_fmt("ClientRuntime started: session=0x{:08X} server={} audio={}ch/{}Hz F={} JB={} slots",
            connect_result_.session_id,
            connect_result_.advertised_udp_address,
            connect_result_.audio_format.channels,
            connect_result_.audio_format.sample_rate,
            connect_result_.frame_count,
            config_.jb_capacity_slots);
        // playback 已启动：若首 callback 已带出实际帧数，立即校正几何地板；
        // 否则（首回调未达）由 poll_control 的 500ms tick 兜底。
        sync_geometric_floor();
        return true;
    }
    return false;
}

void ClientRuntime::stop() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    stop_locked();
}

void ClientRuntime::stop_locked() noexcept
{
    log_debug("ClientRuntime stop requested");
    if (!enter_stopping()) {
        return;
    }

    // 先取消 proto keepalive 探活（join ping 线程；之后不再有 on_control_plane_dead 投递）。
    // ping 线程只经 gate 派发，即使残留也因 detach 被丢弃，无 UAF。
    grpc_.stop_keepalive();

    // 取消设备事件合并窗口（挂起的 handler 以 operation_aborted 返回，不决策）。
    // post 可能抛 std::bad_alloc；本函数是 noexcept，未捕获会直接 terminate
    // （对称 server_runtime 的同构写法）。派发失败只意味着定时器随 ioc 停止，
    // 不影响 teardown 正确性。
    try {
        asio::post(ioc_, [gate = callback_gate_]() noexcept {
            gate->invoke([](ClientRuntime& owner) noexcept {
                owner.device_event_timer_.cancel();
                owner.device_event_pending_ = false;
            });
        });
    } catch (const std::exception& e) {
        log_debug_fmt("ClientRuntime: failed to cancel device event window: {}",
            format_exception_message(e));
    } catch (...) {
        log_debug("ClientRuntime: failed to cancel device event window");
    }

    if (playback_) {
        log_debug("ClientRuntime stopping playback backend");
        playback_->stop();
    }
    log_debug("ClientRuntime stopping UDP transport");
    udp_.stop();
    // 控制面已死时跳过 Disconnect：对端不可达，RPC 必超时，只会白白拖延退出
    // （正常路径仍 best-effort 清理）。
    // UDP 心跳已判死同样跳过：server 大概率已不在（或 UDP 不可达），1s 的
    // Disconnect deadline 会全额打满；残留 session 由 server 侧 5s reaper 兜底。
    // ACK 陈旧（超过 2 个心跳间隔无应答）也跳过：不断重连试探的零散 ACK 之后，
    // 一次 Disconnect 的清理价值抵不上确定的 1s 关机拖尾。age<0（从未收到 ACK）
    // 不跳过：刚启动就 Ctrl+C 时首个 ACK 可能在路上，此时 Disconnect 仍有效。
    const auto ack_age_ms = udp_.heartbeat_ack_age_ms();
    const auto ack_stale = ack_age_ms >= 0
        && ack_age_ms > 2 * config_.heartbeat_handshake_interval.count();
    if (connect_result_.session_id != 0
        && !control_plane_dead_.load(std::memory_order_acquire)
        && !udp_.heartbeat_failed()
        && !ack_stale) {
        log_debug_fmt("ClientRuntime disconnecting session=0x{:08X}", connect_result_.session_id);
        try {
            (void)grpc_.disconnect(connect_result_.session_id);
        } catch (const std::system_error& e) {
            log_debug_fmt("ClientRuntime disconnect threw during stop: code={} message={}",
                e.code().value(), format_system_error_message(e.code()));
        } catch (const std::exception& e) {
            log_debug_fmt("ClientRuntime disconnect threw during stop: {}", format_exception_message(e));
        } catch (...) {
            log_debug("ClientRuntime disconnect threw during stop");
        }
    }
    enter_stopped();
    log_info("ClientRuntime stopped");
}

double ClientRuntime::jb_water_level() const noexcept
{
    return jb_ ? jb_->water_level() : 0.0;
}

std::uint32_t ClientRuntime::jb_used_slots() const noexcept
{
    return jb_ ? jb_->used_slots() : 0;
}

std::uint32_t ClientRuntime::jb_capacity_slots() const noexcept
{
    return jb_ ? jb_->capacity_slots() : 0;
}

std::uint64_t ClientRuntime::jb_reanchor_count() const noexcept
{
    return jb_ ? jb_->reanchor_count() : 0;
}

std::uint64_t ClientRuntime::jb_reanchor_sanity_rejections() const noexcept
{
    return jb_ ? jb_->reanchor_sanity_rejections() : 0;
}

std::uint64_t ClientRuntime::jb_last_reanchor_sequence() const noexcept
{
    if (!jb_ || jb_->reanchor_count() == 0) {
        return 0;
    }
    return jb_->last_reanchor_sequence();
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frame_count)
{
    if (state_.load(std::memory_order_acquire) != RuntimeState::Starting) {
        log_error("ClientRuntime::setup_playback: not in Starting state");
        return false;
    }
    if (!format.is_valid() || frame_count == 0) {
        log_error_fmt("ClientRuntime::setup_playback: invalid geometry (format_valid={} frame_count={})",
            format.is_valid(), frame_count);
        return false;
    }
    const auto frame_bytes = format.frame_bytes();
    if (frame_bytes == 0
        || static_cast<std::size_t>(frame_count)
            > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        log_error_fmt("ClientRuntime::setup_playback: frame geometry overflows (frame_count={} frame_bytes={})",
            frame_count, frame_bytes);
        return false;
    }
    frame_count_ = frame_count;
    frame_bytes_ = frame_bytes;
    audio::JitterBufferConfig cfg;
    cfg.capacity_slots = config_.jb_capacity_slots;
    cfg.format = format;
    cfg.frame_count = frame_count;
    // Phase 2：concealment 由 Runtime 配置决定（组件默认关，产品默认开）。
    cfg.concealment.enabled = config_.jb_pcm_concealment;
    cfg.concealment.max_slots = config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS;
    // 修正拼接 crossfade：组件默认关（v1 硬拼接），产品默认开。
    // --jb-no-splice 关闭（出厂即用；只改输出样本、不动时间轴/target/episode，
    // 出问题关掉重跑即 A/B）。
    cfg.splice.enabled = config_.jb_splice_enabled;
    // Phase 1 自适应起步（细则 §6）：起步 target 取硬下限
    // = max(--jb-min-target, 几何地板 + 1)（见 TargetControllerParams::
    // min_target_slots）。低于地板的起步水位会让锚定后的 lead 立刻落进 normal
    // 区以下触发 FILL（静音等待），等于把启动延迟换成静音，所以地板无条件托底。
    // J 在约 16 个包（≈60ms）内收敛，target 随即涨到稳态值。
    const double packet_ms = static_cast<double>(frame_count) * 1000.0
        / static_cast<double>(format.sample_rate);
    // controller 参数先于 cfg 组装：起步水位需要它的硬下限，而 controller
    // 本身要等 JB 建好才能持有。
    audio::TargetControllerParams controller_params;
    std::uint32_t adaptive_initial_target = 4;
    if (config_.jb_adaptive_target) {
        // target 的结构上限 = 2/3 × capacity，而不是 capacity 本身。
        // 水位带随 target 等比缩放（warning_high ≈ 1.5×target，见下方 band 缩放）：
        // target 顶到 capacity 时整条高水位带落到 ring 之外，DROP 机制失效；
        // consumer 随后把 lead 顶满 ring，新到的包全部撞上未消费槽位被
        // slot-busy 拒收——人为造洞，consumer 再在洞上欠载（极端 gain 实测：
        // busy≈25% rx、underrun≈30%，UDP 零丢包但声音全破）。上 1/3 留给
        // 抖动吸收，与固定模式的 0.6 target / 0.9 ceiling 是同一结构。
        controller_params.capacity_slots = std::max<std::uint32_t>(1,
            static_cast<std::uint32_t>(static_cast<double>(config_.jb_capacity_slots)
                * config::JB_ADAPTIVE_TARGET_CAPACITY_RATIO));
        controller_params.packet_ms = packet_ms;
        controller_params.min_target_slots = config_.jb_min_target_slots;
        // stall 峰值项上限与欠载惩罚步长透传（--jb-stall-peak-cap /
        // --jb-underrun-penalty）：两者都是"分离峰值项/反馈项各自贡献"的对照点。
        // 0 是合法极值（分别 = 关闭峰值项 / 关闭整条反馈闭环），语义见 buffer_config.h。
        controller_params.stall_peak_cap_slots = config_.jb_stall_peak_cap_slots;
        controller_params.underrun_penalty_per_event = config_.jb_underrun_penalty_slots;
        // 回落限速 / 涨后锁跌 / 惩罚累计上限与回落速率：取 buffer_config.h
        // 默认，不经 CLI 暴露（区间很窄，暴露只会制造误调）。
        // 几何地板（见 TargetControllerParams::geometric_floor_slots）：一次 playback
        // callback 消耗的包数。用请求的 callback 帧数（WASAPI 实际周期可能略
        // 大，但 ceil 后同值；取不到实际周期也不至于给出错误量级）。
        // u64 域运算：frames_per_buffer 是 u32（允许 0xFFFFFFFF 级病态请求，
        // WASAPI 侧会另行钳制到 engine 周期），u32 加法先回绕再除会算出 0，
        // 把起步 target 拉到地板下。真值由 sync_geometric_floor 按实际几何回填。
        controller_params.geometric_floor_slots = config_.playback.frames_per_buffer == 0
            ? 0u
            : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                  (static_cast<std::uint64_t>(config_.playback.frames_per_buffer) + frame_count - 1) / frame_count,
                  controller_params.capacity_slots));
        // 记录构造期用的地板口径：sync_geometric_floor 拿实际 callback 帧数
        // 比对，不同才更新（多数情况请求值即实际值，零额外动作）。
        applied_geometric_floor_slots_.store(controller_params.geometric_floor_slots,
            std::memory_order_relaxed);
        // 起步 target = 硬下限 = max(--jb-min-target, 几何地板 + 1)，
        // 再夹到结构上限内，保证 JB 的起步水位带与 controller 一致。
        adaptive_initial_target = audio::TargetController::floor_target(controller_params);
        // 起步 target 也不能越过结构上限：controller 构造时会把它夹到
        // max_target_，这里先夹，保证 JB 的起步水位带与 controller 一致。
        adaptive_initial_target = std::min(
            adaptive_initial_target, controller_params.capacity_slots);
        controller_params.initial_target_slots = adaptive_initial_target;

        // band / startup_level 的自适应映射在 buffer 层共用（apply_adaptive_bands）：
        // 离线回放 harness（tests/audio/jitter_control_replay_test.cpp）必须与生产
        // 用同一份口径，否则"仿真里的带几何"和实际跑的会悄悄分叉。理由与不变量
        // 见该函数上方注释。
        audio::apply_adaptive_bands(cfg, config_.jb_capacity_slots, adaptive_initial_target);
    }
    auto jb = audio::JitterBuffer::create(cfg);
    if (!jb) {
        jb_.reset();
        log_error_fmt("ClientRuntime::setup_playback: JitterBuffer create failed: {}",
            audio::audio_error_name(jb.error()));
        return false;
    }
    jb_ = std::move(*jb);
    // Phase 0 estimator 与 JB 同几何构造（timestamp_rate = sample_rate）。
    // Phase 1 controller 与 estimator 同寿（自适应开时）。
    // stall 门阈值与峰值衰减速率都可配（--jb-stall-threshold / --jb-stall-decay）：
    // 前者是"stall 剔除到底有没有用"的唯一 A/B 手段（≤0 = 回到裸 RFC 3550），
    // 后者决定"峰值记多久"（0 = 永久保持）。语义与极值含义见 buffer_config.h。
    estimator_ = std::make_shared<audio::JitterEstimator>(
        format.sample_rate, frame_count, config_.jb_stall_threshold_packets,
        config_.jb_stall_peak_decay_ms_per_sec);
    controller_.reset();
    if (config_.jb_adaptive_target) {
        controller_ = std::make_shared<audio::TargetController>(controller_params);
        log_debug_fmt(
            "ClientRuntime adaptive target controller: min={} floor={} geometric_floor={} target_max={} fall={:.2f}/s dwell={:.0f}ms penalty={:.2f}(max{} decay{:.2f}/s) stall_cap={:.1f} stall_decay={:.1f}ms/s stall_threshold={:.2f}pkt packet_ms={:.3f}",
            controller_params.min_target_slots,
            controller_->min_target(), controller_params.geometric_floor_slots,
            controller_->max_target(),
            controller_params.fall_rate_slots_per_sec, controller_params.rise_dwell_ms,
            controller_params.underrun_penalty_per_event, controller_params.underrun_penalty_max_slots,
            controller_params.underrun_penalty_decay_slots_per_sec,
            controller_->stall_peak_cap_slots(), config_.jb_stall_peak_decay_ms_per_sec,
            config_.jb_stall_threshold_packets, packet_ms);
    }
    // observer 在 start_receive 之前安装（UdpClient 要求启动前配置）：
    // estimator →（自适应时）controller → jb.set_target_slots() 全链
    // 都在 push strand 上，无跨线程写（JB 侧读原子）。
    udp_.set_arrival_observer(
        [estimator = estimator_, controller = controller_, jb = jb_, packet_ms,
            conceal_on = config_.jb_pcm_concealment,
            last_stalls = std::uint64_t { 0 },
            startup_anchored = false, in_storm = false, last_reanchors = std::uint64_t { 0 },
            reanchor_log = ReanchorLogState { },
            // rejects 只在 AQUA_JB_CONTROL_THREAD_DEBUG_LOG 的分支里被读写；
            // 宏关闭时捕获它会被 clang 报 -Wunused-lambda-capture。lambda 是本 TU
            // 局部的，按同一条件捕获不涉及跨 TU 布局，所以这里可以用 #if。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
            rejects = JbRejectLogState { },
#endif
            jb_trace = config_.jb_packet_trace](
            std::uint16_t sequence, std::uint32_t timestamp,
            std::uint32_t ssrc, std::int64_t arrival_ns) mutable {
            estimator->observe(sequence, timestamp, ssrc, arrival_ns);
            const auto estimates = estimator->estimates();
        // 控制面日志（#5，见本文件顶部说明）：启动 → 稳态的切换时刻。
        // pre-roll 期间 play_seq 恒为 0（只吐静音），锚定后第一拍才进入稳态；
        // 此前这一步没有任何事件可观察，启动窗口与稳态在日志里连成一片。
        // 每会话一次，push strand 上无热点顾虑，不设门。
        if (!startup_anchored && jb->play_sequence() != 0) {
            startup_anchored = true;
            log_debug_fmt(
                "ClientRuntime startup anchored (startup -> steady): play_seq={} lead={}({:.1f}ms)/{} target={} used_slots={} water={:.2f} jit_ms={:.2f} packets={}",
                jb->play_sequence(), jb->lead_slots(),
                static_cast<double>(jb->lead_slots()) * packet_ms, jb->capacity_slots(),
                jb->target_slots(), jb->used_slots(), jb->water_level(),
                estimates.jitter_ms, estimates.packets);
        }
        // 控制面日志（#9）：reanchor 应用边沿。每次应用都扔掉一批已缓存
        // （可闻断裂），值得一行 warn；风暴期按秒汇总（同 #7 节流模式）。
        // RT 侧只记计数（宏门内），这里在 push strand 上读计数做边沿，
        // 不碰 RT 契约。
        {
            const auto reanchors = jb->reanchor_count();
            if (reanchors != last_reanchors) {
                const bool first = !reanchor_log.logged;
                if (first
                    || arrival_ns - reanchor_log.last_log_ns >= kJbRejectLogIntervalNs) {
                    log_warn_fmt(
                        "ClientRuntime reanchor applied: +{} (cum {}) play_seq={} lead={}({:.1f}ms) target={} water={:.2f}",
                        reanchors - reanchor_log.count, reanchors,
                        jb->play_sequence(), jb->lead_slots(),
                        static_cast<double>(jb->lead_slots()) * packet_ms,
                        jb->target_slots(), jb->water_level());
                    reanchor_log.count = reanchors;
                    reanchor_log.last_log_ns = arrival_ns;
                    reanchor_log.logged = true;
                }
                last_reanchors = reanchors;
            }
        }
            if (estimates.stall_events != last_stalls) {
                // stall（时间断流）与抖动分开记：它不进 J，但要让人一眼看到
                // "刚才是断流不是抖动"，否则事后无法解释欠载/reanchor 的来源。
                // stall 间隙会刷进 estimator 的峰值跟踪（stall_peak），由
                // controller 的 margin 峰值项响应——这里一并打出方便对照。
                log_debug_fmt(
                    "ClientRuntime network stall: gap={:.1f}ms ({}ms/pkt) stalls={} jit_ms={:.2f} transit_ms={:.1f} stall_peak_ms={:.1f} - not into J, handled stall-peak margin/underrun feedback/reanchor",
                    estimates.last_stall_gap_ms, packet_ms,
                    estimates.stall_events, estimates.jitter_ms, estimates.transit_ms,
                    estimates.stall_peak_ms);
                last_stalls = estimates.stall_events;
            }
            if (controller != nullptr) {
                const auto previous = controller->current();
                // 细则 §3：欠载历史是 controller 的输入。JB 侧计数器由 RT 线程
                // 写，这里只 relaxed 读快照做增量，不涉及跨线程写。
                // 计罚口径经 select_penalty_events 映射（与离线 harness 同函数）：
                // conceal 开时只有掩盖不住的可闻缺损才抬地板，被盖住的孤立短缺口
                // 不买延迟；conceal 关时退回 underrun_events。
                const auto penalty_events = audio::select_penalty_events(
                    conceal_on, jb->underrun_events(), jb->concealed_saturated_slots());
                const auto target = controller->update(arrival_ns, penalty_events,
                    estimates.stall_peak_ms, estimates.tail_p99_ms, estimates.stall_events);
                jb->set_target_slots(target);
                // 控制面日志（#10）：风暴模式边沿。storm_hold 只在"下跌被冻结"
                // 的拍出现，target 不动时风暴的进出完全隐形——这里按
                // in_storm() 状态做边沿，进出各一行（状态机保证低频，天然节流）。
                {
                    const bool storm_now = controller->in_storm();
                    if (storm_now != in_storm) {
                        in_storm = storm_now;
                        log_debug_fmt(
                            "ClientRuntime storm {}: target={} desired={} stall_events={} penalty={:.2f}",
                            storm_now ? "entered (falls frozen)" : "exited (falls resume)",
                            target, controller->last_desired(), estimates.stall_events,
                            controller->underrun_penalty());
                    }
                }
                if (target != previous) {
                    // 四个水位带整组取一次：target 会随每个包变化，分四次读
                    // 单值会拿到不同快照的带值，日志里的 bands[] 就不可解释了。
                    const auto bands = jb->bands();
                    // 细则 §11：target 为什么变化必须可解释 —— 同一次变化里把
                    // 抖动/stall 峰值/margin 胜出方与夹持状态/欠载反馈/target
                    // 与水位带一起打出来。push strand 上，允许日志。
                    log_debug_fmt(
                        "ClientRuntime adaptive target: {} -> {} slots ({:.1f}ms) jit_ms={:.2f} transit_ms={:.2f} stall_peak_ms={:.1f} src={} floor_bind={} cap_bind={} underrun_penalty={:.2f} bands[wl={} nl={} nh={} wh={}] packet_ms={:.3f}",
                        previous, target, static_cast<double>(target) * packet_ms,
                        estimates.jitter_ms, estimates.transit_ms,
                        estimates.stall_peak_ms,
                        audio::target_margin_source_name(controller->margin_source()),
                        controller->floor_bound() ? 1 : 0,
                        controller->cap_bound() ? 1 : 0,
                        controller->underrun_penalty(),
                        bands.warning_low, bands.normal_low,
                        bands.normal_high, bands.warning_high, packet_ms);
                }
            }

        // 控制面日志（#7，见本文件顶部说明）：push 拒绝的原因分布。计数器要等
        // 1s 的 diag 行才看得到，而 busy=25% 那种病态需要精确定位第一个被拒的
        // 包。首次即时，之后每秒最多一行；行内是"与上次打印之间"的增量。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
            const auto rejected_late = jb->push_rejected_late();
            const auto rejected_busy = jb->push_rejected_slot_busy();
            const auto rejected_invalid = jb->push_rejected_invalid();
            const auto rejected_sanity = jb->push_rejected_sanity();
            const bool reject_changed = rejected_late != rejects.late
                || rejected_busy != rejects.busy
                || rejected_invalid != rejects.invalid
                || rejected_sanity != rejects.sanity;
            if (reject_changed
                && (!rejects.logged
                    || arrival_ns - rejects.last_log_ns >= kJbRejectLogIntervalNs)) {
                log_warn_fmt(
                    "ClientRuntime push rejected{}: +late={} +busy={} +invalid={} +sanity={} (cum late={} busy={} invalid={} sanity={} total={}) play_seq={} highest={} seq={}",
                    rejects.logged ? "" : " (first)",
                    rejected_late - rejects.late, rejected_busy - rejects.busy,
                    rejected_invalid - rejects.invalid, rejected_sanity - rejects.sanity,
                    rejected_late, rejected_busy, rejected_invalid, rejected_sanity,
                    jb->push_rejected(), jb->play_sequence(),
                    jb->highest_received_sequence(), sequence);
                rejects.late = rejected_late;
                rejects.busy = rejected_busy;
                rejects.invalid = rejected_invalid;
                rejects.sanity = rejected_sanity;
                rejects.last_log_ns = arrival_ns;
                rejects.logged = true;
            }
#endif
            // 逐包 trace（--jb-trace，默认关）：放在全链之后，因此 target 是"本包
            // 决策后的值"。字段取够离线重放（seq/ts/arr_ns 足以重建到达节奏），
            // 其余是当时观测与决策，便于直接画图看"P99 动了 target 有没有跟"。
            // 级别是 trace（不是 debug）：274 行/s 的 firehose 不该进默认 debug
            // 视图；采回放语料时用 --log-level trace + --log-file 接住。
            if (jb_trace) {
                // JBT 是离线回放的输入（parser 只认 seq/ts/arr_ns，尾巴字段可增不可减）：
                // p99/tsamp 给尾部深度（门限跨越、冷启动填充），tr/tstep 给路径跳变。
                log_trace_fmt(
                    "JBT seq={} ts={} arr_ns={} jit_ms={:.3f} p99_ms={:.2f} tsamp={} stall_peak_ms={:.1f} target={} lead={} tr_ms={:.2f} trlvl_ms={:.2f} tstep={}",
                    sequence, timestamp, arrival_ns, estimates.jitter_ms,
                    estimates.tail_p99_ms, estimates.tail_samples, estimates.stall_peak_ms,
                    jb->target_slots(), jb->lead_slots(), estimates.transit_ms,
                    estimates.transit_level_ms, estimates.transit_step_events);
            }
        });
    return true;
}

std::uint64_t ClientRuntime::jb_push_accepted() const noexcept { return jb_ ? jb_->push_accepted() : 0; }
std::uint64_t ClientRuntime::jb_push_rejected() const noexcept { return jb_ ? jb_->push_rejected() : 0; }
std::uint64_t ClientRuntime::jb_push_rejected_late() const noexcept { return jb_ ? jb_->push_rejected_late() : 0; }
std::uint64_t ClientRuntime::jb_push_rejected_slot_busy() const noexcept { return jb_ ? jb_->push_rejected_slot_busy() : 0; }
std::uint64_t ClientRuntime::jb_push_rejected_invalid() const noexcept { return jb_ ? jb_->push_rejected_invalid() : 0; }
std::uint64_t ClientRuntime::jb_push_rejected_sanity() const noexcept { return jb_ ? jb_->push_rejected_sanity() : 0; }
std::uint64_t ClientRuntime::jb_pull_calls() const noexcept { return jb_ ? jb_->pull_calls() : 0; }
std::uint64_t ClientRuntime::jb_pull_frames() const noexcept { return jb_ ? jb_->pull_frames() : 0; }
std::uint64_t ClientRuntime::jb_pull_silence_frames() const noexcept { return jb_ ? jb_->pull_silence_frames() : 0; }
std::uint64_t ClientRuntime::jb_fill_episodes() const noexcept { return jb_ ? jb_->fill_episodes() : 0; }
std::uint64_t ClientRuntime::jb_fill_corrected_slots() const noexcept { return jb_ ? jb_->fill_corrected_slots() : 0; }
std::uint64_t ClientRuntime::jb_drop_episodes() const noexcept { return jb_ ? jb_->drop_episodes() : 0; }
std::uint64_t ClientRuntime::jb_drop_skipped_slots() const noexcept { return jb_ ? jb_->drop_skipped_slots() : 0; }
std::uint64_t ClientRuntime::jb_splice_events() const noexcept { return jb_ ? jb_->splice_events() : 0; }
std::uint64_t ClientRuntime::jb_reanchor_requests() const noexcept { return jb_ ? jb_->reanchor_requests() : 0; }
std::uint64_t ClientRuntime::jb_reanchor_cancels() const noexcept { return jb_ ? jb_->reanchor_cancels() : 0; }
std::uint64_t ClientRuntime::playback_pull_calls() const noexcept { return playback_pull_calls_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_frames() const noexcept { return playback_pull_frames_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_silence_frames() const noexcept { return playback_pull_silence_frames_.load(std::memory_order_relaxed); }

void ClientRuntime::latch_audio_error(audio::AudioError error) noexcept
{
    // 同值重复不递增 epoch（错误风暴下 UI 不重复播报同一错误）。
    // 错误值与 epoch 在同一次 CAS 中发布（见 audio_error_state_ 注释）。
    auto cur = audio_error_state_.load(std::memory_order_acquire);
    for (;;) {
        if (static_cast<audio::AudioError>(cur & kAudioErrorMask) == error) {
            return;
        }
        const std::uint64_t next
            = (static_cast<std::uint64_t>(std::to_underlying(error)) & kAudioErrorMask)
            | (((cur >> kAudioErrorEpochShift) + 1) << kAudioErrorEpochShift);
        if (audio_error_state_.compare_exchange_weak(cur, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void ClientRuntime::clear_audio_error() noexcept
{
    auto cur = audio_error_state_.load(std::memory_order_acquire);
    for (;;) {
        if (static_cast<audio::AudioError>(cur & kAudioErrorMask) == audio::AudioError::None) {
            return;
        }
        // 清零 = 低 8 位置 0，epoch 递增（同一次 CAS 发布）。
        const std::uint64_t next = ((cur >> kAudioErrorEpochShift) + 1) << kAudioErrorEpochShift;
        if (audio_error_state_.compare_exchange_weak(cur, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

void ClientRuntime::on_playback_event(audio::AudioError error) noexcept
{
    if (error == audio::AudioError::None) {
        return;
    }
    latch_audio_error(error);

    // 切换事务进行中（Switching）：该错误是事务 stop() 阶段投递的旧流滞留
    // 错误（或切换窗口内旧流的临终错误）。路由由事务自身的候选链负责，
    // 不再派发恢复——否则一次手动切换会叠加一次多余的 restart_on_error，
    // 拉长静音窗口并消耗重试预算。
    if (playback_ != nullptr
        && playback_->state() == audio::PlaybackState::Switching) {
        log_debug_fmt("client runtime: playback event {} during Switching, recovery dispatch skipped (transaction owns routing)",
            audio::audio_error_name(error));
        return;
    }

    // 设备类错误（拔出/不可用/消失）：置标志并立即把派发请求 post 到 ioc
    // （§7：本回调运行在 backend event 线程，restart 的 stop/join/start
    // 不得在此执行；不等待控制线程的下一个 500ms tick——检测延迟是设备
    // 切换静音期的大头）。派发经 callback_gate_（析构时 detach），ioc 上
    // 残留的任务不会触碰已销毁的 runtime；service_playback_recovery 在
    // ioc 线程就地执行 restart 事务（stop+start，JB 不清空 = 结转）。
    if (error == audio::AudioError::DeviceDisconnected
        || error == audio::AudioError::DeviceUnavailable
        || error == audio::AudioError::DeviceNotFound) {
        playback_device_error_pending_.store(true, std::memory_order_release);
        log_warn_fmt("client runtime: device error {}, recovery dispatching",
            audio::audio_error_name(error));
        // post 可能抛 std::bad_alloc；本回调签名是 noexcept，未捕获会 terminate。
        // 派发失败时保留待处理标志，由 supervision tick（500ms）兜底恢复。
        try {
            asio::post(ioc_, [gate = callback_gate_]() noexcept {
                gate->invoke([](ClientRuntime& owner) noexcept {
                    owner.service_playback_recovery();
                });
            });
        } catch (const std::exception& e) {
            log_warn_fmt("client runtime: playback recovery dispatch failed, falling back to supervision tick: {}",
                format_exception_message(e));
        } catch (...) {
            log_warn("client runtime: playback recovery dispatch failed, falling back to supervision tick");
        }
        return;
    }

    // 其余错误（格式/后端内部错误等）：保持既有 Degraded 语义。
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: {}", audio::audio_error_name(error));
                return;
            }
            continue;
        }
        return;
    }
}

void ClientRuntime::service_playback_recovery() noexcept
{
    bool flagged = playback_device_error_pending_.load(std::memory_order_acquire);
    if (flagged
        && last_audio_error() == audio::AudioError::None) {
        // 标志已过期：切换/恢复成功时错误被 clear_audio_error() 清零，但设备
        // 错误的**回调线程**可能迟到，在事务完成之后才置位标志（AAudio 的错误
        // 回调与 switch 事务天然竞态）。错误已清零 = 这件事已经处理完了，再
        // restart 一次只会把刚恢复好的流又拆一遍（多一段静音 + 消耗预算）。
        // 真正的"设备出事"一定是先 latch 错误再置标志，顺序保证不会出现
        // "标志置位但错误为空"的真阳性被误吞。
        // CAS 而非 load+store：backend 事件线程可能在两步之间置位新的真错误，
        // 裸 store(false) 会把这次真错误一并吞掉（只能等下一拍或静默死流兜底）。
        // CAS 失败 = 期间又被置位，保留标志交给本轮后续逻辑处理。
        bool still_set = true;
        if (playback_device_error_pending_.compare_exchange_strong(still_set, false,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            log_debug("client runtime: stale device error flag absorbed (error already cleared)");
            flagged = false;
        }
    }
    // playback_ 的读取必须在 lifecycle_mutex_ 内：它与 destroy()/stop() 并发
    // 时是 use-after-free（CallbackGate 只保护异步 post 路径，supervision tick
    // 与本方法都是直接调用）。锁是 500ms 一次的非竞争临界区，成本可忽略。
    std::lock_guard lock(lifecycle_mutex_);
    if (!playback_) {
        return;
    }
    // 静默死流兜底：PlaybackState 认为 Running 但 backend 已停止消费
    // （如回调线程因未投递的错误退出）。JB 只进不出会被打满且永久静音，
    // 与设备错误同等对待，走同一 restart 事务（受重试预算约束）。
    const bool silent_death = !flagged
        && playback_->state() == audio::PlaybackState::Running
        && !playback_->is_running();
    if (!flagged && !silent_death) {
        return;
    }

    const bool had_flag = playback_device_error_pending_.exchange(false, std::memory_order_acq_rel);
    // 仅会话运行中恢复：Stopping/Stopped/Degraded 交给既有终止路径。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return;
    }
    if (!playback_) {
        return;
    }
    // 锁内双检查：既无标志也不再处于死流状态（可能已被并发恢复），则无事可做。
    if (!had_flag
        && (playback_->state() != audio::PlaybackState::Running || playback_->is_running())) {
        return;
    }
    // 旧流的迟到错误：设备错误往往由 backend 的错误线程**异步**投递，可能在我
    // 们已经完成切换/恢复之后才到（典型：手动从蓝牙切到扬声器，蓝牙流的
    // Disconnected 晚一步）。此时新流健康，再 restart 一次只会制造静音窗口、
    // 消耗 10s/3 次的重试预算，甚至把会话带进 Fatal（用户观感：换个设备结果
    // 整条连接断了）。真正的"正在用的设备出事"一定伴随后端停止消费
    // （is_running() == false），因此不会误吞真阳性。
    if (had_flag && playback_->state() == audio::PlaybackState::Running
        && playback_->is_running()) {
        log_info_fmt("client runtime: device error flag ignored - current stream is healthy "
                     "(stale error from a replaced stream, trigger=device_error)");
        clear_audio_error();
        return;
    }
    log_info_fmt("client runtime: starting playback recovery (trigger={})",
        had_flag ? "device_error" : "silent_stream_death");
    // 链耗尽 → PlaybackState=Fatal；supervision 下一 tick 按 Fatal 终止。
    const auto result = playback_->restart_on_error();
    if (result.has_value()) {
        // 恢复成功（含降级成功）：错误不再是"正在发生"，清零（epoch 递增
        // 通知轮询方）；Fatal 路径保留错误供停止原因查询。
        clear_audio_error();
    }
}

void ClientRuntime::service_default_device_follow() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    // 仅会话运行中跟随；Stopping/Stopped/Degraded 交给既有终止路径。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return;
    }
    if (!playback_) {
        return;
    }
    // 设备轮询与切换决策在 PlaybackManager::tick()（它持 AudioDeviceManager
    // 引用，负责 FollowSystem 的默认设备变化检测）；本方法只做生命周期门禁 +
    // lifecycle_mutex_ 串行化后转发，ClientRuntime 不感知具体设备语义。
    // 跟随事务成功且仍在运行 = 同一次设备变化已被处理完毕：吸收可能待处理的
    // 设备错误标志，否则旧流临终错误会让下一次恢复再做一次多余 restart
    // （对称 server 侧 service_capture_switching 的吸收逻辑）。
    const bool followed = playback_->tick();
    if (followed && playback_->state() == audio::PlaybackState::Running) {
        playback_device_error_pending_.store(false, std::memory_order_release);
        clear_audio_error();
    }
}

void ClientRuntime::sync_geometric_floor() noexcept
{
    // 固定模式 / controller 未建：无事可做。
    if (controller_ == nullptr) {
        return;
    }
    const auto callback_frames = last_callback_frames_.load(std::memory_order_relaxed);
    if (callback_frames == 0 || frame_count_ == 0) {
        return; // 尚无 callback（playback 未起 / 首回调未达），下次 poll 再试
    }
    // u64 域运算：与构造期地板同口径（见上），避免 u32 加法回绕。
    const auto floor = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(callback_frames) + frame_count_ - 1) / frame_count_);
    if (floor == applied_geometric_floor_slots_.load(std::memory_order_relaxed)) {
        return; // 与已应用口径一致：幂等退出
    }
    const auto previous = controller_->min_target();
    controller_->update_geometric_floor(floor);
    applied_geometric_floor_slots_.store(floor, std::memory_order_relaxed);
    log_info_fmt(
        "ClientRuntime geometric floor recalibrated from actual callback geometry: "
        "callback_frames={} frame_count={} floor={} (min_target {} -> {})",
        callback_frames, frame_count_, floor, previous, controller_->min_target());
}

ClientRuntime::ControlPoll ClientRuntime::poll_control() noexcept
{
    // 错误驱动的播放恢复（链耗尽 → Fatal）与默认设备跟随（FollowSystem），
    // 与 service_* 同控制线程串行；随后按终态裁决，stop 由调用方执行。
    service_playback_recovery();
    service_default_device_follow();
    // 设备切换事务可能改变 callback 几何：用实际值校正几何地板。
    sync_geometric_floor();
    if (state_.load(std::memory_order_acquire) == RuntimeState::Degraded) {
        return ControlPoll::StopDegraded;
    }
    if (playback_state() == audio::PlaybackState::Fatal) {
        return ControlPoll::StopFatal;
    }
    return ControlPoll::Continue;
}

void ClientRuntime::notify_devices_changed(
    std::vector<audio::AudioDeviceId> present_ids) noexcept
{
    // 任意线程可调用（Kotlin lifecycle executor / 平台回调线程）：合并去抖
    // 与决策都在 ioc 线程，经 callback_gate_ 保活（析构 detach 后残留任务
    // 不触碰 runtime）。post 可能抛 std::bad_alloc；本函数是 noexcept，
    // 未捕获会 terminate。派发失败时丢弃该份快照——设备快照是幂等的，
    // 后续事件或 FollowSystem 轮询会重新覆盖。
    try {
        asio::post(ioc_,
            [gate = callback_gate_, ids = std::move(present_ids)]() mutable noexcept {
                gate->invoke([&ids, gate](ClientRuntime& owner) noexcept {
                    // 1s 合并窗口（ioc 线程串行）：窗口内最新快照覆盖，到期只
                    // 决策一次（蓝牙连接风暴常伴随多次 add/remove）。
                    owner.pending_device_ids_ = std::move(ids);
                    if (owner.device_event_pending_) {
                        return;
                    }
                    owner.device_event_pending_ = true;
                    owner.device_event_timer_.expires_after(kDeviceEventMergeWindow);
                    owner.device_event_timer_.async_wait(
                        [gate](const asio::error_code& ec) noexcept {
                            if (ec) {
                                return; // 窗口被取消（stop 路径）：不决策
                            }
                            gate->invoke([](ClientRuntime& owner) noexcept {
                                owner.service_devices_changed();
                            });
                        });
                });
            });
    } catch (const std::exception& e) {
        log_warn_fmt("client runtime: device event dispatch failed, snapshot dropped: {}",
            format_exception_message(e));
    } catch (...) {
        log_warn("client runtime: device event dispatch failed, snapshot dropped");
    }
}

void ClientRuntime::service_devices_changed() noexcept
{
    device_event_pending_ = false;
    std::lock_guard lock(lifecycle_mutex_);
    // 非运行期不决策（快照留在 pending_device_ids_；下一份事件重新进入窗口）。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return;
    }
    if (!playback_) {
        return;
    }
    // 路由决策全部在 PlaybackManager（跟随 / eager 回退 / 自动切回）。
    // 触发了事务则吸收可能待处理的设备错误标志：notify 驱动的 eager
    // restart 与错误驱动恢复处理的是同一次设备消失，避免双重 restart。
    if (playback_->on_devices_changed(pending_device_ids_)) {
        playback_device_error_pending_.store(false, std::memory_order_release);
        // 事务完成且仍在运行 = 设备错误已被此次切换处理完毕：清零锁存
        // （epoch 递增通知轮询方）。否则"设备已断开"会作为残值永久挂在
        // 错误通道——错误驱动路径由 service_playback_recovery 成功后清零，
        // notify 驱动路径此前无人清零。时序安全：旧流临终错误在事务
        // stop() 阶段 latch（join 保证先于事务返回），此处必在其后。
        // 链耗尽 → Fatal（非 Running）：保留错误供停止原因查询。
        if (playback_->state() == audio::PlaybackState::Running) {
            clear_audio_error();
        }
    }
}

std::expected<audio::SwitchResult, audio::AudioError>
ClientRuntime::set_playback_device(std::optional<audio::AudioDeviceId> target) noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return std::unexpected(audio::AudioError::NotRunning);
    }
    if (!playback_) {
        return std::unexpected(audio::AudioError::NotRunning);
    }
    log_info_fmt("client runtime: set_playback_device target={} route_mode={} active_device={} jb_water={:.2f} jb_used={}/{}",
        target ? target->value() : std::string("follow_system"),
        audio::playback_route_mode_name(playback_->route_mode()),
        playback_->active_device() ? playback_->active_device()->value() : std::string("unknown"),
        jb_ ? jb_->water_level() : 0.0,
        jb_ ? jb_->used_slots() : 0,
        jb_ ? jb_->capacity_slots() : 0);
    const auto result = playback_->set_playback_device(std::move(target));
    log_info_fmt("client runtime: set_playback_device done: {} jb_water={:.2f} jb_used={}/{}",
        result.has_value() ? "ok" : audio::audio_error_name(result.error()),
        jb_ ? jb_->water_level() : 0.0,
        jb_ ? jb_->used_slots() : 0,
        jb_ ? jb_->capacity_slots() : 0);
    if (result.has_value()) {
        // 切换成功：吸收切换事务期间（以及旧流 stop() 阶段）投递的设备错误
        // 标志——否则下一次 service_playback_recovery 会把它当成"设备出了事"，
        // 在**刚切好的新流**上再做一次多余 restart（stop+start），轻则多一段
        // 静音、重则耗尽重试预算直达 Fatal 把整条连接停掉。与自动跟随路径
        // （service_default_device_follow）的吸收语义对称。
        playback_device_error_pending_.store(false, std::memory_order_release);
        // 用户显式切换成功（含回滚/兜底）：此前的设备错误已被手动恢复覆盖。
        clear_audio_error();
    }
    return result;
}

void ClientRuntime::on_network_liveness_failure(std::uint32_t consecutive_misses) noexcept
{
    // 存活分层、双致命：UDP liveness 在 association 未建立时致命（连不上 server 的
    // UDP 端口，重试无意义）；建立后稳态连续 HEARTBEAT_ACK_MISS_THRESHOLD 个周期
    // 无 ACK 同样致命（server 对每个 heartbeat 都回 ACK，无 ACK 即路径死亡）。
    // 任一死亡都置 Degraded，supervision 观察到后 stop + 退出，不重试。
    const bool associated = udp_.learned_peer_endpoint().has_value();
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (associated) {
                    log_warn_fmt(
                        "client runtime degraded: UDP path dead (no heartbeat ACK for {} intervals, association was established)",
                        consecutive_misses);
                } else {
                    log_warn_fmt("client runtime degraded: no heartbeat ACK for {} consecutive intervals",
                        consecutive_misses);
                }
                return;
            }
            continue;
        }
        return;
    }
}

void ClientRuntime::on_control_plane_dead(grpc::GrpcClient::KeepaliveStatus status) noexcept
{
    // 控制面死亡即 Degraded（supervision 观察到后 stop + 退出），不重试：
    // TCP 断了 ping 救不回来，会话没了重 ping 也没用。幂等 latch。
    control_plane_dead_.store(true, std::memory_order_release);
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: control plane dead ({})",
                    status == grpc::GrpcClient::KeepaliveStatus::SessionGone
                        ? "session gone"
                        : "transport dead");
                return;
            }
            continue;
        }
        return;
    }
}

void ClientRuntime::on_reanchor_sanity_failure(std::uint64_t rejections) noexcept
{
    if (rejections == 0) {
        return;
    }
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: rejected {} absurd JitterBuffer reanchor requests",
                    rejections);
                return;
            }
            continue;
        }
        return;
    }
}

std::uint32_t ClientRuntime::pull_playback(std::span<std::byte> output) noexcept
{
    // 几何地板的权威观测：本次 callback 实际请求的帧数（backend 决定，可能
    // 大于 start 时的请求值）。relaxed 原子缓存，控制线程的
    // sync_geometric_floor 读取后校正 controller。
    if (frame_bytes_ != 0) {
        last_callback_frames_.store(
            static_cast<std::uint32_t>(output.size() / frame_bytes_),
            std::memory_order_relaxed);
    }
    if (jb_ == nullptr) {
        return 0;
    }
    const auto result = jb_->pull(output);
    playback_pull_calls_.fetch_add(1, std::memory_order_relaxed);
    playback_pull_frames_.fetch_add(result.frames_filled, std::memory_order_relaxed);
    playback_pull_silence_frames_.fetch_add(result.silence_frames, std::memory_order_relaxed);
    return result.frames_filled;
}

aqua::diagnostics::ClientDiagnosticsSnapshot ClientRuntime::take_diagnostics_snapshot() const noexcept
{
    // playback_ 的读取必须在 lifecycle_mutex_ 内：它与 destroy()/stop() 并发时
    // 是 use-after-free（CallbackGate 只保护异步 post 路径，而本方法由 C API /
    // JNI 轮询线程**直接**调用，不走 post）。此前本方法漏了这把锁，与
    // service_playback_recovery 等处建立的约定不一致；另外
    // preferred_or_active_device() 会读 std::string，与 switch_to 并发即数据竞争。
    // 本方法不被任何已持该锁的方法调用（调用方只有 C API 两处），无嵌套死锁。
    // 临界区只做快照拷贝，成本可忽略。
    std::lock_guard lock(lifecycle_mutex_);
    aqua::diagnostics::ClientDiagnosticsSnapshot snapshot;
    snapshot.state = state_.load(std::memory_order_acquire);
    snapshot.playback_running = playback_running();
    snapshot.playback_state = playback_state();
    if (playback_ != nullptr) {
        snapshot.route_mode = playback_->route_mode();
        snapshot.switch_result = playback_->last_switch_result().value_or(
            audio::SwitchResult { });
        snapshot.requested_device_id = playback_->preferred_or_active_device().value_or(
            audio::AudioDeviceId { });
    }

    snapshot.net.heartbeat_ack_count = udp_.heartbeat_ack_count();
    snapshot.net.heartbeat_ack_misses = udp_.consecutive_heartbeat_ack_misses();
    snapshot.net.heartbeat_ack_age_ms = udp_.heartbeat_ack_age_ms();
    snapshot.net.heartbeat_failed = udp_.heartbeat_failed();
    snapshot.net.heartbeat_handshake_send_attempts = udp_.heartbeat_handshake_send_attempts();
    snapshot.net.heartbeat_ack_miss_events = udp_.heartbeat_ack_miss_events();
    // Phase 0 观测快照（estimator 缺席如未启动时保持 0）。
    if (estimator_ != nullptr) {
        const auto estimates = estimator_->estimates();
        snapshot.net.estimator_jitter_ms = estimates.jitter_ms;
        snapshot.net.estimator_base_delay_ms = estimates.base_delay_ms;
        snapshot.net.estimator_transit_ms = estimates.transit_ms;
        snapshot.net.estimator_transit_level_ms = estimates.transit_level_ms;
        snapshot.net.estimator_transit_step_events = estimates.transit_step_events;
        snapshot.net.estimator_last_transit_step_ms = estimates.last_transit_step_ms;
        snapshot.net.estimator_reordered_packets = estimates.reordered;
        snapshot.net.estimator_duplicate_packets = estimates.duplicates;
        snapshot.net.estimator_late_packets = estimates.late;
    }
    snapshot.net.transport = udp_.stats();
    snapshot.net.audio_frames_accepted = udp_.audio_frames_accepted();
    snapshot.net.rx_audio_sequence_gap_events = udp_.rx_audio_sequence_gap_events();
    snapshot.net.rx_audio_sequence_missing_frames = udp_.rx_audio_sequence_missing_frames();
    snapshot.net.malformed_datagrams = udp_.malformed_datagrams();
    snapshot.net.unexpected_sender_datagrams = udp_.unexpected_sender_datagrams();
    snapshot.net.wrong_session_acks = udp_.wrong_session_acks();
    snapshot.net.audio_payload_mismatches = udp_.audio_payload_mismatches();
    snapshot.net.non_audio_datagrams = udp_.non_audio_datagrams();

    auto& jb = snapshot.jitter_buffer;
    if (jb_ != nullptr) {
        jb.water_level = jb_->water_level();
        jb.used_slots = jb_->used_slots();
        jb.capacity_slots = jb_->capacity_slots();
        jb.reanchor_count = jb_->reanchor_count();
        jb.reanchor_requests = jb_->reanchor_requests();
        jb.reanchor_cancels = jb_->reanchor_cancels();
        jb.reanchor_sanity_rejections = jb_->reanchor_sanity_rejections();
        jb.last_reanchor_sequence = jb_last_reanchor_sequence();
        jb.push_accepted = jb_->push_accepted();
        jb.push_rejected = jb_->push_rejected();
        jb.push_rejected_late = jb_->push_rejected_late();
        jb.push_rejected_slot_busy = jb_->push_rejected_slot_busy();
        jb.push_rejected_invalid = jb_->push_rejected_invalid();
        jb.push_rejected_sanity = jb_->push_rejected_sanity();
        jb.pull_calls = jb_->pull_calls();
        jb.pull_frames = jb_->pull_frames();
        jb.pull_silence_frames = jb_->pull_silence_frames();
        jb.fill_episodes = jb_->fill_episodes();
        jb.fill_corrected_slots = jb_->fill_corrected_slots();
        jb.drop_episodes = jb_->drop_episodes();
        jb.drop_skipped_slots = jb_->drop_skipped_slots();
        jb.splice_events = jb_->splice_events();
        jb.lead_slots = jb_->lead_slots();
        // Phase 1：当前 target + 实际 lead + estimator jitter 同一快照可读，
        // 可解释 target 为什么变化、JB 为什么没达到 target（细则 §11）。
        jb.target_slots = jb_->target_slots();
        const auto sample_rate = connect_result_.audio_format.sample_rate;
        const double packet_ms = sample_rate > 0 && frame_count_ > 0
            ? static_cast<double>(frame_count_) * 1000.0 / static_cast<double>(sample_rate)
            : 0.0;
        jb.target_ms = static_cast<double>(jb.target_slots) * packet_ms;
        jb.lead_ms = static_cast<double>(jb.lead_slots) * packet_ms;
        jb.play_sequence = jb_->play_sequence();
        jb.highest_received_sequence = jb_->highest_received_sequence();
        jb.consecutive_silence_frames = jb_->consecutive_silence_frames();
        jb.max_silence_run_frames = jb_->max_silence_run_frames();
        jb.episode_state = static_cast<std::int32_t>(jb_->episode_state());
        jb.reanchor_pending = jb_->reanchor_pending();
        jb.reanchor_target_sequence = jb_->reanchor_target_sequence();
        // Phase 2 欠载预算 / concealment（细则 §8/§11：ratio 与单次长度是验收
        // 指标，所以分子分母必须同快照给出）。
        jb.underrun_events = jb_->underrun_events();
        jb.underrun_frames = jb_->underrun_frames();
        jb.max_consecutive_underrun_slots = jb_->max_consecutive_underrun_slots();
        jb.concealed_slots = jb_->concealed_slots();
        jb.concealed_saturated_slots = jb_->concealed_saturated_slots();
        jb.late_useful_packets = jb_->late_useful_packets();
        const auto pull_frames = jb.pull_frames;
        jb.underrun_ratio = pull_frames > 0
            ? static_cast<double>(jb.underrun_frames) / static_cast<double>(pull_frames)
            : 0.0;
        // duty 用帧口径：F 来自会话格式（未建连时 frame_count_ = 0 → 比值 0）。
        const auto slot_frames = static_cast<std::uint64_t>(frame_count_);
        jb.fill_duty = pull_frames > 0
            ? static_cast<double>(jb.fill_corrected_slots * slot_frames) / static_cast<double>(pull_frames)
            : 0.0;
        jb.drop_duty = pull_frames > 0
            ? static_cast<double>(jb.drop_skipped_slots * slot_frames) / static_cast<double>(pull_frames)
            : 0.0;
    }

    // ---- Buffer 决策层观测（末尾追加）----
    // 三部分来源不同，故不塞进上面的 jb/estimator 块：controller（决策层）/
    // estimator（观测层尾部）/ jb（执行层水位带与 run）。controller_ 只在自适应
    // 模式创建（--jb-fixed-target 时为空）→ adaptive=false，其余字段留 0。
    auto& jc = snapshot.jitter_control;
    if (jb_ != nullptr) {
        // 四个带值必须整体取（bands() 单快照）：连着读四个单值 getter 会拿到
        // 不同 target 的带值，诊断就没法解释"为什么 Fill/Drop"。
        const auto bands = jb_->bands();
        jc.band_warning_low = bands.warning_low;
        jc.band_normal_low = bands.normal_low;
        jc.band_normal_high = bands.normal_high;
        jc.band_warning_high = bands.warning_high;
        jc.conceal_run_slots = jb_->conceal_run_slots();
        jc.underrun_run_slots = jb_->underrun_run_slots();
    }
    if (estimator_ != nullptr) {
        const auto estimates = estimator_->estimates();
        jc.stall_events = estimates.stall_events;
        jc.stall_peak_ms = estimates.stall_peak_ms;
        jc.last_stall_gap_ms = estimates.last_stall_gap_ms;
        jc.arrival_interval_ms = estimates.arrival_interval_ms;
    }
    // 切换事务序号（末尾追加）：来源 PlaybackManager，每笔 switch_to 递增。
    // 放在 jb_/estimator_/controller_ 三个判断之外——它只依赖 playback_，固定
    // 模式下同样需要（手动/自动切换在固定模式也会发生）。
    if (playback_ != nullptr) {
        snapshot.switch_seq = playback_->switch_seq();
    }

    if (controller_ != nullptr) {
        jc.adaptive = true;
        jc.desired_slots = controller_->last_desired();
        jc.tail_margin_slots = controller_->last_tail_margin();
        if (estimator_ != nullptr) {
            const auto tail = estimator_->estimates();
            jc.tail_p99_ms = tail.tail_p99_ms;
            jc.tail_samples = tail.tail_samples;
        }
        jc.min_slots = controller_->min_target();
        jc.geometric_floor_slots = applied_geometric_floor_slots_.load(std::memory_order_relaxed);
        jc.max_slots = controller_->max_target();
        jc.margin_source = static_cast<std::int32_t>(controller_->margin_source());
        jc.path = static_cast<std::int32_t>(controller_->path());
        jc.floor_bound = controller_->floor_bound();
        jc.cap_bound = controller_->cap_bound();
        jc.underrun_penalty = controller_->underrun_penalty();
        jc.dwell_remaining_ms = controller_->dwell_remaining_ms();
        jc.fall_room_slots = controller_->fall_room_slots();
    }

    snapshot.playback.pull_calls = playback_pull_calls();
    snapshot.playback.pull_frames = playback_pull_frames();
    snapshot.playback.pull_silence_frames = playback_pull_silence_frames();

    snapshot.stream = playback_ ? playback_->stream_info() : audio::AudioStreamInfo { };
    return snapshot;
}

} // namespace aqua::runtime
