#include "aqua/runtime/client_runtime.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <system_error>

namespace aqua::runtime {

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
    if (config_.jitter_buffer_slots < config::MIN_JITTER_BUFFER_SLOTS
        || config_.jitter_buffer_slots > config::MAX_JITTER_BUFFER_SLOTS) {
        log_error_fmt("ClientRuntime: jitter_buffer_slots must be {}..{}",
            config::MIN_JITTER_BUFFER_SLOTS, config::MAX_JITTER_BUFFER_SLOTS);
        stop_locked();
        return false;
    }

    if (config_.hello_interval <= std::chrono::milliseconds(0)) {
        log_error_fmt("ClientRuntime: invalid configuration: hello_interval={}ms must be > 0",
            config_.hello_interval.count());
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime config: server={} client_name='{}' jitter_slots={} hello_interval={}ms playback_device={} playback_buffer_frames={} playback_low_latency={} prefer_current={}",
        aqua::net::format_host_port(config_.server_ip, config_.rpc_port), config_.client_name, config_.jitter_buffer_slots,
        config_.hello_interval.count(),
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

    log_debug_fmt("ClientRuntime: validated remote stream geometry: payload_bytes={} jitter_slots={}",
        expected_payload_bytes, config_.jitter_buffer_slots);
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
        frame_count_, frame_bytes_, config_.jitter_buffer_slots);

    const auto effective_udp_port = config_.force_udp_port.value_or(connect_result_.advertised_udp_port);
    if (config_.force_udp_port) {
        log_info_fmt("ClientRuntime: overriding Server-advertised UDP port {} with forced port {}",
            connect_result_.advertised_udp_port, *config_.force_udp_port);
    }
    if (!udp_.set_remote(connect_result_.advertised_udp_address, effective_udp_port)) {
        log_error_fmt("ClientRuntime: failed to configure UDP remote {}",
            aqua::net::format_host_port(connect_result_.advertised_udp_address, effective_udp_port));
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime: starting UDP receive, expected payload={} bytes", expected_payload_bytes);
    if (!udp_.start_receive(expected_payload_bytes,
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
            })) {
        log_error("ClientRuntime: failed to start UDP receive loop");
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime UDP receive loop started");
    log_debug_fmt("ClientRuntime: starting heartbeat, handshake interval={}ms",
        config_.hello_interval.count());
    if (!udp_.start_heartbeat(connect_result_.session_id, config_.hello_interval,
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
            config_.jitter_buffer_slots);
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
    if (connect_result_.session_id != 0
        && !control_plane_dead_.load(std::memory_order_acquire)) {
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

double ClientRuntime::jitter_water_level() const noexcept
{
    return jb_ ? jb_->water_level() : 0.0;
}

std::uint32_t ClientRuntime::jitter_used_slots() const noexcept
{
    return jb_ ? jb_->used_slots() : 0;
}

std::uint32_t ClientRuntime::jitter_capacity_slots() const noexcept
{
    return jb_ ? jb_->capacity_slots() : 0;
}

std::uint64_t ClientRuntime::jitter_reanchor_count() const noexcept
{
    return jb_ ? jb_->reanchor_count() : 0;
}

std::uint64_t ClientRuntime::jitter_reanchor_sanity_rejections() const noexcept
{
    return jb_ ? jb_->reanchor_sanity_rejections() : 0;
}

std::uint64_t ClientRuntime::jitter_last_reanchor_sequence() const noexcept
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
    cfg.capacity_slots = config_.jitter_buffer_slots;
    cfg.format = format;
    cfg.frame_count = frame_count;
    // Phase 2：concealment 由 Runtime 配置决定（组件默认关，产品默认开）。
    cfg.concealment.enabled = config_.pcm_concealment;
    cfg.concealment.max_slots = config_.concealment_max_slots;
    // Phase 1 自适应起步（细则 §6）：起步 target 取几何地板
    // （= max(3, 一次 playback callback 的包数+1)），允许高级玩家用
    // `--jb-initial-target` 抬高，但**不允许低于地板**——低于地板的起步水位会
    // 让锚定后的 lead 立刻落进 normal 区以下触发 FILL（静音等待），等于把启动
    // 延迟换成静音。J 在约 16 个包（≈60ms）内收敛，target 随即涨到稳态值。
    constexpr std::uint32_t kAdaptiveStartupSlots = 3;
    // legacy 稳态中心（同时也是固定模式的 target）。自适应模式用它做缩放基准。
    constexpr double kLegacyTarget = 0.60;
    const double packet_ms = static_cast<double>(frame_count) * 1000.0
        / static_cast<double>(format.sample_rate);
    // controller 参数先于 cfg 组装：起步水位需要它的硬下限，而 controller
    // 本身要等 JB 建好才能持有。
    audio::TargetControllerParams controller_params;
    std::uint32_t adaptive_initial_target = 4;
    if (config_.adaptive_jitter) {
        controller_params.capacity_slots = config_.jitter_buffer_slots;
        controller_params.packet_ms = packet_ms;
        controller_params.jitter_gain = config_.jitter_gain;
        controller_params.min_target_slots = config_.min_target_slots;
        controller_params.fall_rate_slots_per_sec = config_.fall_rate_slots_per_sec;
        controller_params.rise_dwell_ms = config_.rise_dwell_ms;
        controller_params.underrun_penalty_per_event = config_.underrun_penalty_slots;
        controller_params.underrun_penalty_max_slots = config_.underrun_penalty_max_slots;
        controller_params.underrun_penalty_decay_slots_per_sec
            = config_.underrun_penalty_decay_slots_per_sec;
        // 几何地板（见 TargetControllerParams::pull_grant_slots）：一次 playback
        // callback 消耗的包数。用请求的 callback 帧数（WASAPI 实际周期可能略
        // 大，但 ceil 后同值；取不到实际周期也不至于给出错误量级）。
        controller_params.pull_grant_slots = config_.playback.frames_per_buffer == 0
            ? 0u
            : (config_.playback.frames_per_buffer + frame_count - 1) / frame_count;
        // 几何地板是硬下限：0（默认）或小到不合理的取值都抬回地板。
        adaptive_initial_target = std::max<std::uint32_t>(
            audio::TargetController::floor_target(controller_params),
            config_.initial_target_slots);
        controller_params.initial_target_slots = adaptive_initial_target;

        const double capacity = static_cast<double>(config_.jitter_buffer_slots);
        // 整条水位带必须随 target 等比缩放，不能只改 target：config 校验强制
        // warning_low < normal_low < target < normal_high < warning_high，
        // 只把 target 折成 4/N（N=30 → 0.133）会小于 normal_low(0.35) 而被
        // 拒绝 —— 自适应模式会直接起不来（JB create 返回 invalid_argument）。
        // 等比缩放既满足严格序，又保持与固定模式相同的 band/target 倍率。
        const double target_ratio = std::min(
            static_cast<double>(adaptive_initial_target) / capacity, kLegacyTarget);
        const double scale = target_ratio / kLegacyTarget; // (0,1]：warning_high 因此恒 <= 0.9
        cfg.target = target_ratio;
        cfg.warning_low = 0.20 * scale;
        cfg.normal_low = 0.35 * scale;
        cfg.normal_high = 0.80 * scale;
        cfg.warning_high = 0.90 * scale;
        // 启动水位（细则 §6）：max(3, 起步 target) slots，但不高于 target。
        // 起步水位若低于 target，锚定后立刻落进 normal 区以下触发 FILL
        // （静音等待），等于把启动延迟换成静音——不如直接按 target 起步。
        const std::uint32_t adaptive_startup_slots
            = std::max<std::uint32_t>(kAdaptiveStartupSlots, adaptive_initial_target);
        cfg.startup_level = std::min(
            static_cast<double>(adaptive_startup_slots) / capacity, target_ratio);
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
    estimator_ = std::make_shared<audio::JitterEstimator>(
        format.sample_rate, frame_count, config_.stall_threshold_packets);
    controller_.reset();
    if (config_.adaptive_jitter) {
        controller_ = std::make_shared<audio::TargetController>(controller_params);
        log_debug_fmt(
            "ClientRuntime adaptive target controller: gain={:.2f} min={} floor={} pull_grant={} fall={:.2f}/s dwell={:.0f}ms penalty={:.2f}(max{} decay{:.2f}/s) packet_ms={:.3f}",
            controller_params.jitter_gain, controller_params.min_target_slots,
            controller_->min_target(), controller_params.pull_grant_slots,
            controller_params.fall_rate_slots_per_sec, controller_params.rise_dwell_ms,
            controller_params.underrun_penalty_per_event, controller_params.underrun_penalty_max_slots,
            controller_params.underrun_penalty_decay_slots_per_sec, packet_ms);
    }
    // observer 在 start_receive 之前安装（UdpClient 要求启动前配置）：
    // estimator →（自适应时）controller → jb.set_target_slots() 全链
    // 都在 push strand 上，无跨线程写（JB 侧读原子）。
    udp_.set_arrival_observer(
        [estimator = estimator_, controller = controller_, jb = jb_, packet_ms,
            last_stalls = std::uint64_t { 0 }](
            std::uint16_t sequence, std::uint32_t timestamp,
            std::uint32_t ssrc, std::int64_t arrival_ns) mutable {
            estimator->observe(sequence, timestamp, ssrc, arrival_ns);
            const auto estimates = estimator->estimates();
            if (estimates.stall_events != last_stalls) {
                // stall（时间断流）与抖动分开记：它不进 J，但要让人一眼看到
                // "刚才是断流不是抖动"，否则事后无法解释欠载/reanchor 的来源。
                log_debug_fmt(
                    "ClientRuntime network stall: gap={:.1f}ms ({}ms/包) stalls={} jit_ms={:.2f} transit_ms={:.1f} — 不进 J，由欠载反馈/reanchor 负责",
                    estimates.last_stall_gap_ms, packet_ms,
                    estimates.stall_events, estimates.jitter_ms, estimates.transit_ms);
                last_stalls = estimates.stall_events;
            }
            if (controller != nullptr) {
                const auto previous = controller->current();
                // 细则 §3：欠载历史是 controller 的输入。JB 侧计数器由 RT 线程
                // 写，这里只 relaxed 读快照做增量，不涉及跨线程写。
                const auto target = controller->update(estimates.base_delay_ms,
                    estimates.jitter_ms, arrival_ns, jb->underrun_events());
                jb->set_target_slots(target);
                if (target != previous) {
                    // 细则 §11：target 为什么变化必须可解释 —— 同一次变化里把
                    // 抖动/底噪/欠载反馈/target 与水位带一起打出来。push
                    // strand 上，允许日志。
                    log_debug_fmt(
                        "ClientRuntime adaptive target: {} -> {} slots ({:.1f}ms) jit_ms={:.2f} base_ms={:.2f} transit_ms={:.2f} underrun_penalty={:.2f} bands[wl={} nl={} nh={} wh={}] packet_ms={:.3f}",
                        previous, target, static_cast<double>(target) * packet_ms,
                        estimates.jitter_ms, estimates.base_delay_ms, estimates.transit_ms,
                        controller->underrun_penalty(),
                        jb->warning_low_slots(), jb->normal_low_slots(),
                        jb->normal_high_slots(), jb->warning_high_slots(), packet_ms);
                }
            }
        });
    return true;
}

std::uint64_t ClientRuntime::jitter_push_accepted() const noexcept { return jb_ ? jb_->push_accepted() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected() const noexcept { return jb_ ? jb_->push_rejected() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_late() const noexcept { return jb_ ? jb_->push_rejected_late() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_slot_busy() const noexcept { return jb_ ? jb_->push_rejected_slot_busy() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_invalid() const noexcept { return jb_ ? jb_->push_rejected_invalid() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_sanity() const noexcept { return jb_ ? jb_->push_rejected_sanity() : 0; }
std::uint64_t ClientRuntime::jitter_pull_calls() const noexcept { return jb_ ? jb_->pull_calls() : 0; }
std::uint64_t ClientRuntime::jitter_pull_frames() const noexcept { return jb_ ? jb_->pull_frames() : 0; }
std::uint64_t ClientRuntime::jitter_pull_silence_frames() const noexcept { return jb_ ? jb_->pull_silence_frames() : 0; }
std::uint64_t ClientRuntime::jitter_fill_episodes() const noexcept { return jb_ ? jb_->fill_episodes() : 0; }
std::uint64_t ClientRuntime::jitter_fill_corrected_slots() const noexcept { return jb_ ? jb_->fill_corrected_slots() : 0; }
std::uint64_t ClientRuntime::jitter_drop_episodes() const noexcept { return jb_ ? jb_->drop_episodes() : 0; }
std::uint64_t ClientRuntime::jitter_drop_skipped_slots() const noexcept { return jb_ ? jb_->drop_skipped_slots() : 0; }
std::uint64_t ClientRuntime::jitter_reanchor_requests() const noexcept { return jb_ ? jb_->reanchor_requests() : 0; }
std::uint64_t ClientRuntime::jitter_reanchor_cancels() const noexcept { return jb_ ? jb_->reanchor_cancels() : 0; }
std::uint64_t ClientRuntime::playback_pull_calls() const noexcept { return playback_pull_calls_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_frames() const noexcept { return playback_pull_frames_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_silence_frames() const noexcept { return playback_pull_silence_frames_.load(std::memory_order_relaxed); }

void ClientRuntime::latch_audio_error(audio::AudioError error) noexcept
{
    // 同值重复不递增 epoch（错误风暴下 UI 不重复播报同一错误）。
    auto expected = last_audio_error_.load(std::memory_order_acquire);
    for (;;) {
        if (expected == error) {
            return;
        }
        if (last_audio_error_.compare_exchange_weak(expected, error,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            audio_error_epoch_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
    }
}

void ClientRuntime::clear_audio_error() noexcept
{
    auto expected = last_audio_error_.load(std::memory_order_acquire);
    for (;;) {
        if (expected == audio::AudioError::None) {
            return;
        }
        if (last_audio_error_.compare_exchange_weak(expected, audio::AudioError::None,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            audio_error_epoch_.fetch_add(1, std::memory_order_acq_rel);
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
    const bool flagged = playback_device_error_pending_.load(std::memory_order_acquire);
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

ClientRuntime::ControlPoll ClientRuntime::poll_control() noexcept
{
    // 错误驱动的播放恢复（链耗尽 → Fatal）与默认设备跟随（FollowSystem），
    // 与 service_* 同控制线程串行；随后按终态裁决，stop 由调用方执行。
    service_playback_recovery();
    service_default_device_follow();
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
    aqua::diagnostics::ClientDiagnosticsSnapshot snapshot;
    snapshot.state = state_.load(std::memory_order_acquire);
    snapshot.playback_running = playback_running();
    snapshot.playback_state = playback_state();
    if (playback_ != nullptr) {
        snapshot.route_mode = playback_->route_mode();
        snapshot.switch_result = playback_->last_switch_result().value_or(
            audio::SwitchResult { });
        snapshot.requested_device_id = playback_->requested_device().value_or(
            audio::AudioDeviceId { });
    }

    snapshot.net.hello_ack_count = udp_.hello_ack_count();
    snapshot.net.hello_ack_misses = udp_.consecutive_hello_ack_misses();
    snapshot.net.hello_ack_age_ms = udp_.hello_ack_age_ms();
    snapshot.net.hello_failed = udp_.hello_failed();
    snapshot.net.hello_send_attempts = udp_.hello_send_attempts();
    snapshot.net.hello_ack_miss_events = udp_.hello_ack_miss_events();
    // Phase 0 观测快照（estimator 缺席如未启动时保持 0）。
    if (estimator_ != nullptr) {
        const auto estimates = estimator_->estimates();
        snapshot.net.estimator_jitter_ms = estimates.jitter_ms;
        snapshot.net.estimator_base_delay_ms = estimates.base_delay_ms;
        snapshot.net.estimator_transit_ms = estimates.transit_ms;
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
        jb.last_reanchor_sequence = jitter_last_reanchor_sequence();
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

    snapshot.playback.pull_calls = playback_pull_calls();
    snapshot.playback.pull_frames = playback_pull_frames();
    snapshot.playback.pull_silence_frames = playback_pull_silence_frames();

    snapshot.stream = playback_ ? playback_->stream_info() : audio::AudioStreamInfo { };
    return snapshot;
}

} // namespace aqua::runtime
