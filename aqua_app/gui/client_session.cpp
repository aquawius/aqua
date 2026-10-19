#include "client_session.h"

#include "log_buffer.h"

#include <aqua/logger/logger.h>
#include <aqua/runtime/client_runtime.h>

#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace aqua::gui {
namespace {

using namespace std::chrono_literals;

// 自动重连的**基础**等待时长由 SessionConfig::reconnect_delay_s 决定（设置 →
// 连接，1–10 秒，默认 3 秒——与 Android 侧同口径）；实际等待按次数指数退避。
constexpr int kMaxReconnectAttempts = 20; // 超过就停在 Failed，不再无限重试
constexpr int kMaxReconnectDelaySeconds = 15; // 退避上限

// 第 attempt 次（从 1 开始）重连前要等多久：base、2×base、4×base… 封顶 15 s。
int reconnect_backoff_seconds(int base_s, int attempt)
{
    int delay = std::max(1, base_s);
    for (int i = 1; i < attempt && delay < kMaxReconnectDelaySeconds; ++i) {
        delay *= 2;
    }
    return std::min(delay, kMaxReconnectDelaySeconds);
}

// 诊断快照 + poll_control 周期（与 CLI 的 RUNTIME_CONTROL_POLL_INTERVAL 一致）。
constexpr auto kControlTick = aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL;
// 可中断睡眠的分片长度：断开请求最多延迟一个分片被观察到。
constexpr auto kSleepSlice = 100ms;

// 可中断睡眠（重连等待期间响应 disconnect）。
void sleep_interruptible(const std::atomic<bool>& stop, std::chrono::milliseconds total)
{
    for (std::chrono::milliseconds slept { 0 }; slept < total; slept += kSleepSlice) {
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        std::this_thread::sleep_for(std::min(kSleepSlice, total - slept));
    }
}

std::string endpoint_text(const SessionConfig& config)
{
    return config.rpc_ip + ":" + std::to_string(config.rpc_port);
}

// 清空"本次会话的固定信息"（音频格式 / 会话号 / 数据面）。离开 Connected 时调，
// 让 UI 回到占位态，而不是停在上一场会话的格式串上。调用方必须持 mutex_。
void clear_session_facts(SessionSnapshot& snapshot)
{
    snapshot.session_id = 0;
    snapshot.data_plane.clear();
    snapshot.audio_format.clear();
    snapshot.sample_rate = 0;
    snapshot.channels = 0;
    snapshot.encoding.clear();
    snapshot.bits_per_sample = 0;
    snapshot.bitrate_kbps = 0;
    snapshot.frame_count = 0;
    snapshot.packet_ms = 0.0;
}

// 编码标签 / 位深，与 Android 的 AquaEncoding 保持一致（PCM_F32LE → "PCM F32LE", 32）。
const char* encoding_label(aqua::audio::AudioEncoding enc)
{
    switch (enc) {
    case aqua::audio::AudioEncoding::PCM_S16LE: return "PCM S16LE";
    case aqua::audio::AudioEncoding::PCM_S32LE: return "PCM S32LE";
    case aqua::audio::AudioEncoding::PCM_F32LE: return "PCM F32LE";
    case aqua::audio::AudioEncoding::PCM_S24LE: return "PCM S24LE";
    case aqua::audio::AudioEncoding::PCM_U8:    return "PCM U8";
    default:                                    return "—";
    }
}

const char* channels_label(std::uint32_t channels)
{
    switch (channels) {
    case 1:  return "单声道";
    case 2:  return "立体声";
    default: return "多声道";
    }
}

// 音频格式摘要（遥测面板第二行）。字段与 Android 的「音频格式」卡一致：
//   采样率 · 声道 · 编码 · 位深 · 码率 · 帧长
std::string audio_format_text(const aqua::grpc::ConnectResult& result)
{
    const aqua::audio::AudioFormat& f = result.audio_format;
    if (!f.is_valid()) {
        return {};
    }
    const std::uint32_t bytes = f.bytes_per_sample();
    const std::uint32_t rate = f.sample_rate;
    const std::uint32_t ch = f.channels;

    std::string text = (rate % 1000 == 0)
        ? std::to_string(rate / 1000) + " kHz"
        : std::to_string(rate) + " Hz";
    text += std::string(" · ") + channels_label(ch);
    text += std::string(" · ") + encoding_label(f.encoding);
    // 位深不单列：编码名里已经带了（F32LE = 32 bit），遥测面板只有 ~67 个等宽
    // 字符的宽度，每一列都要算钱。
    if (bytes > 0) {
        // PCM 码率 = 采样率 × 声道 × 字节数 × 8
        const auto kbps = static_cast<unsigned long long>(rate) * ch * bytes * 8 / 1000;
        text += " · " + std::to_string(kbps) + " kbps";
    }
    if (result.frame_count > 0) {
        char tail[48];
        std::snprintf(tail, sizeof(tail), " · %u 帧 ≈ %.1f ms", result.frame_count,
            static_cast<double>(result.frame_count) * 1000.0 / static_cast<double>(rate));
        text += tail;
    }
    return text;
}

// 连接成功后的状态行：会话号 + 服务端通告的数据面地址 + 音频格式。
std::string connected_text(const SessionConfig& config,
    const aqua::grpc::ConnectResult& result)
{
    std::string text = "已连接 " + endpoint_text(config);
    if (result.session_id != 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08X", result.session_id);
        text += "（会话 0x";
        text += hex;
        if (!result.advertised_udp_address.empty()) {
            text += "，数据面 ";
            text += result.advertised_udp_address + ":" + std::to_string(result.advertised_udp_port);
        }
        if (result.audio_format.sample_rate != 0) {
            text += "，" + std::to_string(result.audio_format.sample_rate) + "Hz/"
                + std::to_string(result.audio_format.channels) + "ch";
        }
        text += "）";
    }
    return text;
}

} // namespace

ClientSession::ClientSession() = default;

ClientSession::~ClientSession()
{
    stop_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        if (io_context_ != nullptr) {
            io_context_->stop();
        }
        // 析构路径允许等待：进程正在退出，阻塞到 worker 收尾是可接受的。
        if (runtime_ != nullptr) {
            runtime_->stop();
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ClientSession::connect(const SessionConfig& config)
{
    if (active_.load(std::memory_order_acquire)) {
        return false; // 已有会话在跑（含断开中）
    }
    if (worker_.joinable()) {
        worker_.join(); // 上一轮已结束，回收线程
    }
    stop_requested_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    publish(UiPhase::Connecting, "正在连接 " + endpoint_text(config));
    // 不再额外写 "client: connecting"：core 自己会打
    // "ClientRuntime: connecting to gRPC server …"，两行说同一件事是噪音。
    worker_ = std::thread([this, config] { worker_main(config); });
    return true;
}

void ClientSession::disconnect()
{
    // 没有 worker 在跑（从未连接 / 已收尾 / 连接失败后）→ 什么都不做。
    // 早先这里无条件 publish(Disconnecting)，失败后再点断开会把阶段永久卡在
    // "断开中…"（探针实测），因为没有任何线程会再发布 Idle。
    if (!active_.load(std::memory_order_acquire)) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    std::shared_ptr<aqua::runtime::ClientRuntime> runtime;
    std::shared_ptr<asio::io_context> ioc;
    {
        std::lock_guard lock(mutex_);
        runtime = runtime_;
        ioc = io_context_;
    }
    // 已连接时 stop() 很快（取消 I/O），可以从 UI 线程调。
    // 握手期（start 阻塞在 gRPC 上）不能调：stop() 会等 start() 让出
    // lifecycle_mutex_，UI 就冻住了——那种情况只置 stop_requested_，由 worker
    // 在 start() 返回后自己收尾。这里只压 io_context，让已排队的 handler 立刻
    // 退出（不会打断正在跑的 start()）。
    if (runtime != nullptr && !handshaking_.load(std::memory_order_acquire)) {
        runtime->stop();
    }
    if (ioc != nullptr) {
        ioc->stop();
    }
    publish(UiPhase::Disconnecting, "正在断开…");
}

SessionSnapshot ClientSession::poll() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool ClientSession::set_playback_device(std::optional<aqua::audio::AudioDeviceId> target)
{
    // 运行期切换输出设备（连接卡片的下拉框，播放中点哪台就切到哪台）。
    // 与 C API / Android 的做法同构：UI 线程直接调 core 的
    // ClientRuntime::set_playback_device——它内部走 lifecycle_mutex_ 串行化，
    // 且只在 Running 状态接受；完整候选链（目标 → 上一个 → 系统默认）与回滚都在
    // PlaybackManager 里，UI 不需要（也不该）自己实现降级逻辑。
    // runtime_ 用 shared_ptr 拷出来再调：worker 收尾与本次调用并发时对象不会被
    // 就地析构（同 disconnect() 的理由）。
    std::shared_ptr<aqua::runtime::ClientRuntime> runtime;
    {
        std::lock_guard lock(mutex_);
        runtime = runtime_;
    }
    if (runtime == nullptr) {
        return false;
    }
    const auto result = runtime->set_playback_device(std::move(target));
    if (!result.has_value()) {
        // NotRunning = 连接已断 / 还没起来：调用方（卡片）自己按阶段决定要不要提示，
        // 这里只把"没切成"的事实返回。
        return false;
    }
    return true;
}

void ClientSession::notify_devices_changed(std::vector<aqua::audio::AudioDeviceId> present)
{
    // 热插拔：把"现在都有哪些输出设备"告诉 core（与 C API / JNI 同一入口）。
    // Windows 上以前没有任何人推——只有 Android 的 AudioManager 回调在推，
    // 所以桌面端拔掉正在用的设备时，core 只能等下一次错误才发现，而"跟随系统
    // 默认"也无法主动换端点。GUI 每 5 s 枚举一次设备，集合变化就在这里推一次。
    std::shared_ptr<aqua::runtime::ClientRuntime> runtime;
    {
        std::lock_guard lock(mutex_);
        runtime = runtime_;
    }
    if (runtime == nullptr) {
        return; // 未连接：没有消费者，下次连接会重新枚举
    }
    runtime->notify_devices_changed(std::move(present));
}

void ClientSession::worker_main(SessionConfig config)
{
    // UI 侧配置 → core 配置（一次转换，重连复用同一个 runtime_config）。
    // JB 参数与播放路由都是"连接属性"：ClientRuntime 构造时固化，运行期不可改
    // ——这正是连接期间锁住这些控件的原因。
    aqua::runtime::ClientRuntimeConfig runtime_config;
    runtime_config.rpc_ip = config.rpc_ip;
    runtime_config.rpc_port = config.rpc_port;
    runtime_config.udp_force_port = config.udp_force_port;
    runtime_config.client_name = config.client_name;
    runtime_config.playback.route = (config.pin_device && !config.device_id.empty())
        ? aqua::audio::AudioRoute::pin(config.device_id)
        : aqua::audio::AudioRoute::follow_system();
    runtime_config.jb_capacity_slots = config.jb.capacity;
    runtime_config.jb_min_target_slots = config.jb.min_target;
    runtime_config.jb_stall_peak_cap_slots = config.jb.stall_peak_cap;
    runtime_config.jb_stall_peak_decay_ms_per_sec = config.jb.stall_decay;
    runtime_config.jb_stall_threshold_packets = config.jb.stall_threshold;
    runtime_config.jb_underrun_penalty_slots = config.jb.underrun_penalty;
    runtime_config.jb_adaptive_target = config.jb_adaptive_target;
    runtime_config.jb_pcm_concealment = config.jb_concealment;
    runtime_config.jb_splice_enabled = config.jb_splice;
    runtime_config.jb_packet_trace = config.jb_packet_trace;
    runtime_config.playback.frames_per_buffer =
        config.playback_buffer_frames > 0
        ? static_cast<std::uint32_t>(config.playback_buffer_frames)
        : 0u;

    int reconnect_attempt = 0;

    for (;;) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            publish(UiPhase::Idle, "已断开");
            break;
        }

        // 上一次会话异常终止 → 等一个间隔再重建（期间仍可被 disconnect 打断）。
        if (reconnect_attempt > 0) {
            // 封顶：以前这里是无上限的 for(;;)，服务器一直不可达就会一直重试，
            // 而 UI 在那个阶段既停不下来也改不了地址（只能杀进程）。20 次 ×
            // 退避（3/6/12/15…）≈ 4 分钟，之后停在 Failed 让用户自己决定。
            if (reconnect_attempt > kMaxReconnectAttempts) {
                publish(UiPhase::Failed,
                    "自动重连已停止（" + std::to_string(kMaxReconnectAttempts) + " 次均失败）："
                    "请检查服务器地址 / 网络后手动重试");
                aqua::log_error("client: giving up after "
                    + std::to_string(kMaxReconnectAttempts) + " reconnect attempts");
                break;
            }
            const int delay_s = reconnect_backoff_seconds(config.reconnect_delay_s, reconnect_attempt);
            publish(UiPhase::Reconnecting,
                "会话中断，" + std::to_string(delay_s) + " 秒后重连（第 "
                    + std::to_string(reconnect_attempt) + "/" + std::to_string(kMaxReconnectAttempts)
                    + " 次）");
            sleep_interruptible(stop_requested_, std::chrono::seconds(delay_s));
            if (stop_requested_.load(std::memory_order_acquire)) {
                publish(UiPhase::Idle, "已断开");
                break;
            }
        }

        // 每次尝试都重建 io_context + ClientRuntime：ClientRuntime 的生命周期是
        // 一次性的（Created → … → Stopped），重连就是重来一轮。
        auto ioc = std::make_shared<asio::io_context>();
        auto runtime = std::make_shared<aqua::runtime::ClientRuntime>(*ioc, runtime_config);
        {
            std::lock_guard lock(mutex_);
            runtime_ = runtime;
            io_context_ = ioc;
        }

        // 首次连接的"正在连接 …"由 connect() 立即发布（UI 要立刻有反馈）；
        // 重连场景在这里补一条，带上第几次。
        if (reconnect_attempt > 0) {
            publish(UiPhase::Connecting,
                "正在重连 " + endpoint_text(config) + "（第 "
                    + std::to_string(reconnect_attempt) + "/"
                    + std::to_string(kMaxReconnectAttempts) + " 次）");
        }

        handshaking_.store(true, std::memory_order_release);
        const bool started = runtime->start();
        handshaking_.store(false, std::memory_order_release);

        if (!started) {
            // 用户在这轮握手期间点了取消：按"已断开"收尾，不要报"连接失败"
            //（那是我们把人家要连的东西掐了，不是服务器的问题）。
            if (stop_requested_.load(std::memory_order_acquire)) {
                clear_runtime_handles();
                publish(UiPhase::Idle, "已断开");
                break;
            }
            if (reconnect_attempt == 0) {
                // 首次连接失败（拒绝 / 超时 / 配置被拒）不重试：这是服务器不可达
                // 或参数问题，自动重试只会让用户莫名等待；日志里已有具体原因。
                publish(UiPhase::Failed,
                    "连接失败：" + endpoint_text(config) + "（详见运行日志）");
                aqua::log_error("client: connect failed: " + endpoint_text(config));
                clear_runtime_handles();
                break;
            }
            // 自动重连中的一次失败：服务器多半还没回来（重启 / 网络恢复中）。
            // 继续按间隔重试——用户随时可以点"断开"终止；放弃才是最差的选择。
            clear_runtime_handles();
            ++reconnect_attempt;
            continue;
        }

        // 连接成功但用户在握手期间已请求断开 → 直接收尾。
        if (stop_requested_.load(std::memory_order_acquire)) {
            runtime->stop();
            clear_runtime_handles();
            publish(UiPhase::Idle, "已断开");
            break;
        }

        // 会话固定信息先落到快照里（遥测面板直接读，不从 status_detail 反解析），
        // 再发布 Connected —— 同一条线程，UI 不可能看到半份状态。
        const aqua::grpc::ConnectResult result = runtime->connect_result();
        const std::string plane = result.advertised_udp_address.empty()
            ? std::string()
            : result.advertised_udp_address + ":" + std::to_string(result.advertised_udp_port);
        {
            const aqua::audio::AudioFormat& f = result.audio_format;
            const std::uint32_t bytes_per_sample = f.bytes_per_sample();
            std::lock_guard lock(mutex_);
            snapshot_.session_id = result.session_id;
            snapshot_.data_plane = plane;
            snapshot_.audio_format = audio_format_text(result);
            snapshot_.sample_rate = static_cast<int>(f.sample_rate);
            snapshot_.channels = static_cast<int>(f.channels);
            snapshot_.encoding = encoding_label(f.encoding);
            snapshot_.bits_per_sample = static_cast<int>(bytes_per_sample) * 8;
            // PCM 码率 = 采样率 × 声道 × 字节数 × 8（与 audio_format_text 同一口径）
            snapshot_.bitrate_kbps = f.sample_rate > 0
                ? static_cast<int>(static_cast<unsigned long long>(f.sample_rate)
                      * f.channels * bytes_per_sample * 8
                      / 1000ULL)
                : 0;
            snapshot_.frame_count = static_cast<int>(result.frame_count);
            snapshot_.packet_ms = f.sample_rate > 0
                ? static_cast<double>(result.frame_count) * 1000.0 / static_cast<double>(f.sample_rate)
                : 0.0;
        }
        publish(UiPhase::Connected, connected_text(config, result));
        // 只补一条 core 不会打的信息（数据面地址）；会话号 / 采样率 core 的
        // "ClientRuntime started" 行里都有，重复一遍只是噪音。
        if (!plane.empty()) {
            aqua::log_info("client: data plane " + plane);
        }

        // ---- 控制循环：500ms 一拍，先发诊断快照再裁决终态 ----
        auto terminal = aqua::runtime::ClientRuntime::ControlPoll::Continue;
        auto timer = std::make_shared<asio::steady_timer>(*ioc);
        std::function<void(const asio::error_code&)> tick;
        tick = [this, &terminal, &tick, timer, runtime, ioc, reconnect_attempt](
                   const asio::error_code& ec) {
            if (ec || stop_requested_.load(std::memory_order_acquire)) {
                return; // ioc 停止 / 用户断开：不再续拍，run() 随后返回
            }
            publish_diagnostics(runtime->take_diagnostics_snapshot(), reconnect_attempt);
            const auto verdict = runtime->poll_control();
            if (verdict != aqua::runtime::ClientRuntime::ControlPoll::Continue) {
                terminal = verdict;
                runtime->stop();
                ioc->stop();
                return;
            }
            timer->expires_after(kControlTick);
            timer->async_wait(tick);
        };
        tick(asio::error_code { });
        ioc->run();
        runtime->stop(); // 幂等：终态裁决已经停过也不会重复做事
        clear_runtime_handles();

        if (stop_requested_.load(std::memory_order_acquire)) {
            publish(UiPhase::Idle, "已断开");
            break;
        }

        const char* reason = (terminal == aqua::runtime::ClientRuntime::ControlPoll::StopDegraded)
            ? "网络中断"
            : "播放故障";
        if (!config.auto_reconnect) {
            publish(UiPhase::Failed, std::string("会话已断开：") + reason);
            break;
        }
        ++reconnect_attempt; // 回到循环顶部，走重连等待
    }

    active_.store(false, std::memory_order_release);
}

void ClientSession::publish(UiPhase phase, std::string detail)
{
    {
        std::lock_guard lock(mutex_);
        snapshot_.phase = phase;
        snapshot_.status_detail = detail;
        ++snapshot_.snapshot_seq; // UI 趋势缓冲据此判断"有新的一帧"
        // 离开 Connected 就丢掉诊断与会话固定信息：指标条显示 "-"、音频格式行显示
        // "—"，而不是停在断线前那一场会话的数字 / 格式串（用户 2026-10-18 反馈：
        // "音频格式连接后再断开不会清空"）。
        if (phase != UiPhase::Connected) {
            snapshot_.has_diagnostics = false;
            clear_session_facts(snapshot_);
        }
    }
    // 注意：这里**不写日志**。detail 是给人和状态栏看的中文，而运行日志面板要
    // 呈现的是与终端一致的 spdlog 输出（英文、统一格式）。会话层的关键事件在
    // 各转换点单独写英文行（见 worker_main / connect / disconnect）。
}

void ClientSession::publish_diagnostics(
    const aqua::diagnostics::ClientDiagnosticsSnapshot& snapshot, int reconnect_attempt)
{
    // 到达速率：core 只给累计收包数，速率是本拍与上一拍的差值 / 实际间隔。
    // 在这里（worker 线程、500ms 一拍）算，UI 侧就不用跨帧推算。
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t packets = snapshot.net.transport.rx_packets;
    if (rx_rate_has_base_) {
        const double dt = std::chrono::duration<double>(now - rx_rate_last_at_).count();
        const double dp = static_cast<double>(packets) - static_cast<double>(rx_rate_last_packets_);
        if (dp < 0.0) {
            rx_rate_ema_ = 0.0; // 计数器回退 = 新会话
        } else if (dt > 1.0e-3) {
            const double instant = dp / dt;
            rx_rate_ema_ = (rx_rate_ema_ > 0.0) ? (rx_rate_ema_ * 0.6 + instant * 0.4) : instant;
        }
    }
    rx_rate_last_packets_ = packets;
    rx_rate_last_at_ = now;
    rx_rate_has_base_ = true;

    std::lock_guard lock(mutex_);
    snapshot_.has_diagnostics = true;
    snapshot_.diagnostics = snapshot;
    snapshot_.reconnect_attempt = reconnect_attempt;
    snapshot_.rx_rate = rx_rate_ema_;
    ++snapshot_.snapshot_seq;
}

void ClientSession::clear_runtime_handles()
{
    std::lock_guard lock(mutex_);
    runtime_.reset();
    io_context_.reset();
}

ClientSession& client_session()
{
    static ClientSession session;
    return session;
}

void apply_session_snapshot(const SessionSnapshot& snapshot, UiState& state)
{
    // 演示模式（AQUA_GUI_DEMO）不消费会话快照：它的 phase 由 demo 自己维护
    // （draw_main_window 首次置 Connected、toggle_connection 翻转），指标走
    // fill_demo_tiles。否则每帧被这里的 Idle 覆盖——界面预览会变成"未连接"。
    if (state.demo) {
        return;
    }
    state.phase = snapshot.phase;
    state.session.status_detail = snapshot.status_detail;
    state.session.reconnect_attempt = snapshot.reconnect_attempt;
    state.session.live = snapshot.has_diagnostics;
    state.session.session_id = snapshot.session_id;
    state.session.data_plane = snapshot.data_plane;
    state.session.audio_format = snapshot.audio_format;
    state.session.sample_rate = snapshot.sample_rate;
    state.session.channels = snapshot.channels;
    state.session.encoding = snapshot.encoding;
    state.session.bits_per_sample = snapshot.bits_per_sample;
    state.session.bitrate_kbps = snapshot.bitrate_kbps;
    state.session.frame_count = snapshot.frame_count;
    state.session.packet_ms = snapshot.packet_ms;
    state.session.snapshot_seq = snapshot.snapshot_seq;
    state.session.rx_rate = snapshot.rx_rate;
    // 完整诊断整份透传（设置→统计的详表）：live=false 时它是残留值，
    // 统计页以 live 为门，不在这里清零——遥测面板还留着会话固定信息展示。
    state.session.diag = snapshot.diagnostics;
    if (!snapshot.has_diagnostics) {
        return;
    }
    const auto& diag = snapshot.diagnostics;
    state.session.water_level = diag.jitter_buffer.water_level;
    state.session.lead_ms = diag.jitter_buffer.lead_ms;
    state.session.target_ms = diag.jitter_buffer.target_ms;
    state.session.jitter_ms = diag.net.estimator_jitter_ms;
    // 缺口率 = missing / (accepted + missing)。口径是"接收序列缺口"（乱序与丢包
    // 都会计入），core 刻意不把它叫丢包率——UI 的标签同口径。
    const std::uint64_t accepted = diag.net.audio_frames_accepted;
    const std::uint64_t missing = diag.net.rx_audio_sequence_missing_frames;
    state.session.gap_ratio = (accepted + missing) > 0
        ? static_cast<double>(missing) / static_cast<double>(accepted + missing)
        : 0.0;
    state.session.underrun_ratio = diag.jitter_buffer.underrun_ratio;

    // ---- 第二轮诊断扩充（core 2026-10-19）：整组透传，UI 只显示不推演 ----
    {
        const auto& src = diag;
        auto& dst = state.session.diag_ex;
        dst.engine_period_frames = src.stream_ex.engine_period_frames;
        dst.engine_period_min_frames = src.stream_ex.engine_period_min_frames;
        dst.engine_period_max_frames = src.stream_ex.engine_period_max_frames;
        dst.engine_period_default_frames = src.stream_ex.engine_period_default_frames;
        dst.engine_period_fundamental_frames = src.stream_ex.engine_period_fundamental_frames;
        dst.engine_resampling = src.stream_ex.engine_resampling;
        dst.mix_sample_rate = src.stream_ex.mix_sample_rate;
        dst.mix_channels = src.stream_ex.mix_channels;
        dst.buffer_size_frames = src.stream_ex.buffer_size_frames;
        dst.output_buffering_frames = src.stream_ex.output_buffering_frames;

        dst.stale_events_dropped = src.playback_ex.stale_events_dropped;
        dst.empty_event_skips = src.playback_ex.empty_event_skips;
        dst.catchup_frames = src.playback_ex.catchup_frames;
        dst.catchup_frames_max = src.playback_ex.catchup_frames_max;
        dst.callback_frames = src.playback_ex.callback_frames;
        dst.sample_rate_adjusted = src.playback_ex.sample_rate_adjusted;

        dst.rx_consecutive_errors = src.net_ex.rx_consecutive_errors;
        dst.rx_last_error_code = src.net_ex.rx_last_error_code;
        dst.estimator_packets = src.net_ex.estimator_packets;
        dst.estimator_gap_events = src.net_ex.estimator_gap_events;
        dst.estimator_missing_packets = src.net_ex.estimator_missing_packets;
        dst.estimator_base_set = src.net_ex.estimator_base_set;
        dst.udp_address_from_sentinel = src.net_ex.udp_address_from_sentinel;
        dst.udp_family = src.net_ex.udp_family;
        dst.learned_peer_endpoint = src.net_ex.learned_peer_endpoint;

        dst.oldest_received_sequence = src.jitter_buffer_ex.oldest_received_sequence;
        dst.conceal_enabled = src.jitter_buffer_ex.conceal_enabled;
        dst.conceal_max_slots = src.jitter_buffer_ex.conceal_max_slots;
        dst.splice_enabled = src.jitter_buffer_ex.splice_enabled;
        dst.splice_xfade_frames = src.jitter_buffer_ex.splice_xfade_frames;
        dst.startup_slots = src.jitter_buffer_ex.startup_slots;
        dst.initial_target_slots = src.jitter_buffer_ex.initial_target_slots;

        dst.effective_min_slots = src.jitter_control_ex.effective_min_slots;
        dst.in_storm = src.jitter_control_ex.in_storm;
        dst.last_stall_margin_slots = src.jitter_control_ex.last_stall_margin_slots;
        dst.stall_peak_cap_slots = src.jitter_control_ex.stall_peak_cap_slots;
        dst.fall_rate_slots_per_sec = src.jitter_control_ex.fall_rate_slots_per_sec;

        dst.remote_sample_rate = src.remote.sample_rate;
        dst.remote_channels = src.remote.channels;
        dst.remote_encoding = src.remote.encoding;
        dst.remote_bitrate_kbps = src.remote.bitrate_kbps;
        dst.remote_frame_count = src.remote.frame_count;
    }
}

} // namespace aqua::gui
