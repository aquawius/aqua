// Aqua client C API 实现：ClientRuntime 的薄 wrapper + 监督线程。
// 业务、生命周期、音频路径全部在 aqua::runtime::ClientRuntime；
// 本文件只负责 C 边界转换与 CLI control timer 语义的线程化。

#include "aqua/c_api/aqua_capi.h"

#include "aqua/logger/logger.h"
#include "aqua/runtime/client_runtime.h"
#include "aqua/version.h"

#include <asio.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

// ---- 枚举镜像数值与 core C++ 枚举强一致（编译期契约，全值锁定）----
#define AQUA_CAPI_ASSERT_ENUM_MIRROR(cpp_enum, c_value) \
    static_assert(static_cast<int>(cpp_enum) == c_value, #cpp_enum " != " #c_value)

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Created, AQUA_STATE_CREATED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Starting, AQUA_STATE_STARTING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Running, AQUA_STATE_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Degraded, AQUA_STATE_DEGRADED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Stopping, AQUA_STATE_STOPPING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Stopped, AQUA_STATE_STOPPED);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackState::Inactive, AQUA_PLAYBACK_INACTIVE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackState::Starting, AQUA_PLAYBACK_STARTING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackState::Running, AQUA_PLAYBACK_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackState::Switching, AQUA_PLAYBACK_SWITCHING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackState::Fatal, AQUA_PLAYBACK_FATAL);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackRouteMode::FollowSystem, AQUA_ROUTE_FOLLOW_SYSTEM);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackRouteMode::PreferCurrent, AQUA_ROUTE_PREFER_CURRENT);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::PlaybackRouteMode::PreferredDevice, AQUA_ROUTE_PREFERRED_DEVICE);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::SwitchOutcome::None, AQUA_SWITCH_NONE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::SwitchOutcome::Switched, AQUA_SWITCH_SWITCHED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::SwitchOutcome::RolledBack, AQUA_SWITCH_ROLLED_BACK);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::SwitchOutcome::FellBackToSystem, AQUA_SWITCH_FELL_BACK_TO_SYSTEM);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::SwitchOutcome::Fatal, AQUA_SWITCH_FATAL);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::None, AQUA_AUDIO_NONE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::DeviceNotFound, AQUA_AUDIO_DEVICE_NOT_FOUND);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::DeviceUnavailable, AQUA_AUDIO_DEVICE_UNAVAILABLE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::DeviceDisconnected, AQUA_AUDIO_DEVICE_DISCONNECTED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::FormatUnsupported, AQUA_AUDIO_FORMAT_UNSUPPORTED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::NotSupported, AQUA_AUDIO_NOT_SUPPORTED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::PermissionDenied, AQUA_AUDIO_PERMISSION_DENIED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::AlreadyRunning, AQUA_AUDIO_ALREADY_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::NotRunning, AQUA_AUDIO_NOT_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::InvalidArgument, AQUA_AUDIO_INVALID_ARGUMENT);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::audio::AudioError::BackendFailed, AQUA_AUDIO_BACKEND_FAILED);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Trace, AQUA_LOG_TRACE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Debug, AQUA_LOG_DEBUG);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Info, AQUA_LOG_INFO);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Warn, AQUA_LOG_WARN);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Error, AQUA_LOG_ERROR);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::LogLevel::Fatal, AQUA_LOG_FATAL);

#undef AQUA_CAPI_ASSERT_ENUM_MIRROR

namespace {

// core 日志只初始化一次：init_logger() 非幂等（每次替换 spdlog 默认 logger），
// C API 允许多次 create/destroy，必须以 call_once 守护。
std::once_flag g_logger_once;

aqua::LogLevel capi_log_level(int32_t level)
{
    switch (level) {
    case AQUA_LOG_TRACE:
        return aqua::LogLevel::Trace;
    case AQUA_LOG_DEBUG:
        return aqua::LogLevel::Debug;
    case AQUA_LOG_INFO:
        return aqua::LogLevel::Info;
    case AQUA_LOG_WARN:
        return aqua::LogLevel::Warn;
    case AQUA_LOG_ERROR:
        return aqua::LogLevel::Error;
    case AQUA_LOG_FATAL:
        return aqua::LogLevel::Fatal;
    default:
        return aqua::LogLevel::Info;
    }
}

} // namespace

// ---- C API handle：core 对象 + IO/监督线程 ----

struct aqua_client {
    asio::io_context ioc;
    aqua::runtime::ClientRuntimeConfig config;
    std::unique_ptr<aqua::runtime::ClientRuntime> runtime;
    std::jthread io_thread;

    explicit aqua_client(aqua::runtime::ClientRuntimeConfig cfg)
        : config(std::move(cfg))
    {
    }

    // CLI control timer 的等价物：500ms 监督 tick（playback_switching_design.md §6）：
    //   RuntimeState::Degraded（网络/控制面）→ stop()   [含 proto keepalive 判死]
    //   PlaybackState::Fatal → stop()                    [链耗尽]
    //   heartbeat_failed → 不动作（与 Degraded 同 tick 锁存；UDP 路径死亡与控制面
    //     死亡都经 handler 置 Degraded，本监督只看 Degraded）
    //   Switching / 设备错误 → 不动作（错误驱动的恢复在下方先执行）
    // 运行在 io_thread 上（唯一 ioc.run() 调用者）。
    void supervision_main()
    {
        auto timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> tick;
        tick = [this, timer, &tick](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            // 错误驱动的播放恢复 + 默认设备跟随 + 终态裁决（单实现见
            // ClientRuntime::poll_control；Android 设备跟随为 no-op，
            // 由 notify_devices_changed 推送驱动）。
            const auto verdict = runtime->poll_control();
            if (verdict != aqua::runtime::ClientRuntime::ControlPoll::Continue) {
                const auto snapshot = runtime->take_diagnostics_snapshot();
                aqua::log_debug_fmt("capi: supervision observed terminal condition: state={} heartbeat_failed={} playback_state={}",
                    aqua::runtime::runtime_state_name(snapshot.state), snapshot.net.heartbeat_failed,
                    aqua::audio::playback_state_name(snapshot.playback_state));
                runtime->stop();
                ioc.stop();
                return;
            }
            timer->expires_after(aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL);
            timer->async_wait(tick);
        };
        tick(asio::error_code { });
        ioc.run();
    }
};

// ---- 版本 / 枚举名 ----

const char* aqua_version(void)
{
    return AQUA_CORE_VERSION;
}

const char* aqua_runtime_state_name(int state)
{
    // 文案唯一真相源是 aqua::runtime::runtime_state_name；此处只做 int 翻译，
    // 不再存第二份映射表（加枚举值只需改 C++ 侧 + 下方镜像断言）。
    return aqua::runtime::runtime_state_name(
        static_cast<aqua::runtime::RuntimeState>(state));
}

const char* aqua_audio_error_name(int error)
{
    // 同上，真相源是 aqua::audio::audio_error_name。
    return aqua::audio::audio_error_name(static_cast<aqua::audio::AudioError>(error));
}

// ---- 生命周期 ----

aqua_client_t* aqua_client_create(const aqua_client_config_t* config)
{
    if (config == nullptr || config->server_ip == nullptr || config->server_ip[0] == '\0') {
        return nullptr;
    }

    std::call_once(g_logger_once, [] {
        aqua::init_logger();
        aqua::set_log_level(aqua::default_log_level());
    });
    if (config->log_level >= 0) {
        aqua::set_log_level(capi_log_level(config->log_level));
    }

    aqua::runtime::ClientRuntimeConfig cfg;
    cfg.server_ip = config->server_ip;
    cfg.rpc_port = config->rpc_port != 0 ? config->rpc_port : aqua::config::DEFAULT_RPC_PORT;
    if (config->client_name != nullptr && config->client_name[0] != '\0') {
        cfg.client_name = config->client_name;
    }
    if (config->jb_capacity_slots != 0) {
        cfg.jb_capacity_slots = config->jb_capacity_slots;
    }
    if (config->heartbeat_handshake_interval_ms != 0) {
        cfg.heartbeat_handshake_interval = std::chrono::milliseconds(config->heartbeat_handshake_interval_ms);
    }
    if (config->playback_frames_per_buffer != 0) {
        cfg.playback.frames_per_buffer = config->playback_frames_per_buffer;
    } else {
        // 0 = backend 自行决定（aqua_capi.h 契约）：显式透传 0，而不是保留 480 默认，
        // 否则 AAudio 永远收到显式 480 容量（"不显式设置"设计被架空）。
        cfg.playback.frames_per_buffer = 0;
    }
    if (config->udp_force_port != 0) {
        cfg.udp_force_port = config->udp_force_port;
    }
    cfg.playback.low_latency = config->playback_low_latency != 0;
    cfg.playback_prefer_current = config->playback_prefer_current != 0;
    // 起步目标播放设备（初始化首流前选定）：非空字符串才生效。
    if (config->playback_device_id != nullptr && config->playback_device_id[0] != '\0') {
        cfg.playback.device = aqua::audio::AudioDeviceId(config->playback_device_id);
    }
    // 0 = 自适应开（默认）；非 0 = 固定 target 既有行为。
    cfg.jb_adaptive_target = config->jb_fixed_target == 0;
    // 0 = PCM concealment 开（默认）；非 0 = 关闭（缺帧静音 v1 行为）。
    cfg.jb_pcm_concealment = config->jb_disable_concealment == 0;
    // JB 自适应微调（与 CLI --jb-jitter-gain / --jb-min-target 对齐）：
    // gain 0 / 负 / 非有限 = 默认 5.0（zero-init 惯例，见头文件契约）；
    // min-target 0 = 默认 3，显式值经 controller 的几何地板托底与容量钳制
    // 兜住，无需在此校验区间。
    if (config->jb_jitter_gain > 0.0 && std::isfinite(config->jb_jitter_gain)) {
        cfg.jb_jitter_gain = config->jb_jitter_gain;
    }
    if (config->jb_min_target_slots != 0) {
        cfg.jb_min_target_slots = config->jb_min_target_slots;
    }
    // Wave A 现场调优旋钮（--jb-stall-peak-cap / --jb-stall-decay /
    // --jb-stall-threshold / --jb-underrun-penalty 的 C API 镜像）：zero-init
    // 惯例同上——0 / 负值 / 非有限 = core 默认值，Kotlin 侧不填即保持既有行为。
    // 0 的极值语义（关闭对应机制）只在 CLI 暴露，理由见 aqua_capi.h。
    if (config->jb_stall_peak_cap_slots > 0.0 && std::isfinite(config->jb_stall_peak_cap_slots)) {
        cfg.jb_stall_peak_cap_slots = config->jb_stall_peak_cap_slots;
    }
    if (config->jb_stall_peak_decay_ms_per_sec > 0.0
        && std::isfinite(config->jb_stall_peak_decay_ms_per_sec)) {
        cfg.jb_stall_peak_decay_ms_per_sec = config->jb_stall_peak_decay_ms_per_sec;
    }
    if (config->jb_stall_threshold_packets > 0.0
        && std::isfinite(config->jb_stall_threshold_packets)) {
        cfg.jb_stall_threshold_packets = config->jb_stall_threshold_packets;
    }
    if (config->jb_underrun_penalty_slots > 0.0
        && std::isfinite(config->jb_underrun_penalty_slots)) {
        cfg.jb_underrun_penalty_slots = config->jb_underrun_penalty_slots;
    }

    // unique_ptr 中转 + catch：ClientRuntime 构造可能抛出（UdpClient 等成员
    // 分配失败）；handle 由 RAII 自动释放，异常不得越过 C 边界。
    try {
        std::unique_ptr<aqua_client_t> client = std::make_unique<aqua_client_t>(std::move(cfg));
        client->runtime = std::make_unique<aqua::runtime::ClientRuntime>(client->ioc, client->config);
        aqua::log_debug("capi: client handle created");
        return client.release();
    } catch (...) {
        return nullptr;
    }
}

int aqua_client_start(aqua_client_t* client)
{
    try {
        if (client == nullptr || client->runtime == nullptr) {
            return AQUA_ERR_INVALID_ARGUMENT;
        }
        // 单飞语义（见头文件契约）：已有监督线程在跑时拒绝二次启动。
        // 正常路径下 runtime 的 Created→Starting CAS 已保证二次 start() 恒失败，
        // 这里是纵深防御——一旦 joinable 线程被 move-assign 覆盖就是 std::terminate，
        // 无恢复余地，必须在赋值前拦截。
        if (client->io_thread.joinable()) {
            aqua::log_error("capi: client start rejected: supervision thread already running");
            return AQUA_ERR_INVALID_ARGUMENT;
        }
        if (!client->runtime->start()) {
            aqua::log_debug("capi: client start failed; handle is now Stopped, destroy to retry");
            return AQUA_ERR_START_FAILED;
        }

        try {
            client->io_thread = std::jthread([client](std::stop_token st) {
                // stop 请求 → ioc.stop()：supervision_main 阻塞在 ioc.run()，不打断则
                // jthread 析构的 auto-join 会挂死。ioc 声明在 io_thread 之前，析构安全。
                std::stop_callback cb(st, [client] { client->ioc.stop(); });
                client->supervision_main();
            });
        } catch (...) {
            // 线程创建失败：runtime 已 Running 但无人驱动 io_context（heartbeat 定时器
            // 不会走）。按失败处理，回滚到 Stopped。
            client->runtime->stop();
            return AQUA_ERR_START_FAILED;
        }
        return AQUA_OK;
    } catch (...) {
        aqua::log_error("capi: aqua_client_start aborted by C++ exception");
        return AQUA_ERR_START_FAILED;
    }
}

int aqua_client_stop(aqua_client_t* client)
{
    if (client == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        client->runtime->stop();
        client->ioc.stop();
        client->io_thread.request_stop();
        if (client->io_thread.joinable()) {
            client->io_thread.join();
        }
        return AQUA_OK;
    } catch (...) {
        // join() 等可能抛 system_error；destroy 路径依赖本函数不抛（否则
        // delete 被跳过 → 句柄泄漏），必须兜住。
        aqua::log_error("capi: aqua_client_stop aborted by C++ exception");
        return AQUA_ERR_INTERNAL;
    }
}

void aqua_client_destroy(aqua_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    (void)aqua_client_stop(client);
    delete client;
    aqua::log_debug("capi: client handle destroyed");
}

// ---- 查询 ----

int aqua_client_get_state(const aqua_client_t* client)
{
    if (client == nullptr || client->runtime == nullptr) {
        return -1;
    }
    return static_cast<int>(client->runtime->state());
}

int aqua_client_get_last_audio_error(const aqua_client_t* client)
{
    if (client == nullptr || client->runtime == nullptr) {
        return -1;
    }
    return static_cast<int>(client->runtime->last_audio_error());
}

uint64_t aqua_client_get_audio_error_epoch(const aqua_client_t* client)
{
    if (client == nullptr || client->runtime == nullptr) {
        return 0;
    }
    return client->runtime->audio_error_epoch();
}

int aqua_client_get_diagnostics(const aqua_client_t* client,
    aqua_client_diagnostics_t* out)
{
    try {
        if (client == nullptr || client->runtime == nullptr || out == nullptr) {
            return AQUA_ERR_INVALID_ARGUMENT;
        }
        const auto s = client->runtime->take_diagnostics_snapshot();
        out->state = static_cast<int32_t>(s.state);
        out->playback_running = s.playback_running ? 1 : 0;
        out->playback_state = static_cast<int32_t>(s.playback_state);
        out->route_mode = static_cast<int32_t>(s.route_mode);
        out->switch_outcome = static_cast<int32_t>(s.switch_result.outcome);
        out->switch_error = static_cast<int32_t>(s.switch_result.last_error);
        out->switch_duration_ms = s.switch_result.duration_ms;
        // 截断保护：设备 id 缓冲 AQUA_DEVICE_ID_BYTES，含结尾 NUL。
        std::snprintf(out->requested_device_id, sizeof(out->requested_device_id), "%s",
            s.requested_device_id.value().c_str());
        std::snprintf(out->stream_device_id, sizeof(out->stream_device_id), "%s",
            s.stream.device_id.value().c_str());

        out->net.rx_packets = s.net.transport.rx_packets;
        out->net.rx_bytes = s.net.transport.rx_bytes;
        out->net.rx_errors = s.net.transport.rx_errors;
        out->net.tx_packets = s.net.transport.tx_packets;
        out->net.tx_bytes = s.net.transport.tx_bytes;
        out->net.tx_errors = s.net.transport.tx_errors;
        out->net.tx_dropped = s.net.transport.tx_dropped;
        out->net.tx_enqueue_failures = s.net.transport.tx_enqueue_failures;
        out->net.tx_queue_depth = s.net.transport.tx_queue_depth;
        out->net.heartbeat_ack_count = s.net.heartbeat_ack_count;
        out->net.heartbeat_ack_misses = s.net.heartbeat_ack_misses;
        out->net.heartbeat_ack_age_ms = s.net.heartbeat_ack_age_ms;
        out->net.heartbeat_handshake_send_attempts = s.net.heartbeat_handshake_send_attempts;
        out->net.heartbeat_ack_miss_events = s.net.heartbeat_ack_miss_events;
        out->net.audio_frames_accepted = s.net.audio_frames_accepted;
        out->net.rx_audio_sequence_gap_events = s.net.rx_audio_sequence_gap_events;
        out->net.rx_audio_sequence_missing_frames = s.net.rx_audio_sequence_missing_frames;
        out->net.malformed_datagrams = s.net.malformed_datagrams;
        out->net.unexpected_sender_datagrams = s.net.unexpected_sender_datagrams;
        out->net.wrong_session_acks = s.net.wrong_session_acks;
        out->net.audio_payload_mismatches = s.net.audio_payload_mismatches;
        out->net.non_audio_datagrams = s.net.non_audio_datagrams;
        out->net.heartbeat_failed = s.net.heartbeat_failed ? 1 : 0;

        out->jitter_buffer.water_level = s.jitter_buffer.water_level;
        out->jitter_buffer.used_slots = s.jitter_buffer.used_slots;
        out->jitter_buffer.capacity_slots = s.jitter_buffer.capacity_slots;
        out->jitter_buffer.reanchor_count = s.jitter_buffer.reanchor_count;
        out->jitter_buffer.reanchor_requests = s.jitter_buffer.reanchor_requests;
        out->jitter_buffer.reanchor_cancels = s.jitter_buffer.reanchor_cancels;
        out->jitter_buffer.reanchor_sanity_rejections = s.jitter_buffer.reanchor_sanity_rejections;
        out->jitter_buffer.last_reanchor_sequence = s.jitter_buffer.last_reanchor_sequence;
        out->jitter_buffer.push_accepted = s.jitter_buffer.push_accepted;
        out->jitter_buffer.push_rejected = s.jitter_buffer.push_rejected;
        out->jitter_buffer.push_rejected_late = s.jitter_buffer.push_rejected_late;
        out->jitter_buffer.push_rejected_slot_busy = s.jitter_buffer.push_rejected_slot_busy;
        out->jitter_buffer.push_rejected_invalid = s.jitter_buffer.push_rejected_invalid;
        out->jitter_buffer.push_rejected_sanity = s.jitter_buffer.push_rejected_sanity;
        out->jitter_buffer.pull_calls = s.jitter_buffer.pull_calls;
        out->jitter_buffer.pull_frames = s.jitter_buffer.pull_frames;
        out->jitter_buffer.pull_silence_frames = s.jitter_buffer.pull_silence_frames;
        out->jitter_buffer.fill_episodes = s.jitter_buffer.fill_episodes;
        out->jitter_buffer.fill_corrected_slots = s.jitter_buffer.fill_corrected_slots;
        out->jitter_buffer.drop_episodes = s.jitter_buffer.drop_episodes;
        out->jitter_buffer.drop_skipped_slots = s.jitter_buffer.drop_skipped_slots;
        out->jitter_buffer.lead_slots = s.jitter_buffer.lead_slots;
        out->jitter_buffer.play_sequence = s.jitter_buffer.play_sequence;
        out->jitter_buffer.highest_received_sequence = s.jitter_buffer.highest_received_sequence;
        out->jitter_buffer.consecutive_silence_frames = s.jitter_buffer.consecutive_silence_frames;
        out->jitter_buffer.max_silence_run_frames = s.jitter_buffer.max_silence_run_frames;
        out->jitter_buffer.episode_state = s.jitter_buffer.episode_state;
        out->jitter_buffer.reanchor_pending = s.jitter_buffer.reanchor_pending ? 1 : 0;
        out->jitter_buffer.reanchor_target_sequence = s.jitter_buffer.reanchor_target_sequence;

        out->playback.pull_calls = s.playback.pull_calls;
        out->playback.pull_frames = s.playback.pull_frames;
        out->playback.pull_silence_frames = s.playback.pull_silence_frames;

        out->stream.backend = static_cast<uint32_t>(s.stream.backend);
        out->stream.sample_rate = s.stream.sample_rate;
        out->stream.channels = s.stream.channels;
        out->stream.performance_mode = s.stream.performance_mode;
        out->stream.frames_per_burst = s.stream.frames_per_burst;
        out->stream.buffer_capacity_frames = s.stream.buffer_capacity_frames;
        out->stream.callback_count = s.stream.callback_count;
        out->stream.current_padding_frames = s.stream.current_padding_frames;
        out->stream.xrun_count = s.stream.xrun_count;
        // Phase 0 观测（末尾追加，与 C 结构体顺序一致）。
        out->estimator_jitter_ms = s.net.estimator_jitter_ms;
        out->estimator_base_delay_ms = s.net.estimator_base_delay_ms;
        out->estimator_transit_ms = s.net.estimator_transit_ms;
        out->estimator_reordered_packets = s.net.estimator_reordered_packets;
        out->estimator_duplicate_packets = s.net.estimator_duplicate_packets;
        out->estimator_late_packets = s.net.estimator_late_packets;
        out->target_slots = s.jitter_buffer.target_slots;
        out->target_ms = s.jitter_buffer.target_ms;
        // Phase 2 欠载预算 + concealment
        out->underrun_events = s.jitter_buffer.underrun_events;
        out->underrun_frames = s.jitter_buffer.underrun_frames;
        out->max_consecutive_underrun_slots = s.jitter_buffer.max_consecutive_underrun_slots;
        out->concealed_slots = s.jitter_buffer.concealed_slots;
        out->concealed_saturated_slots = s.jitter_buffer.concealed_saturated_slots;
        out->late_useful_packets = s.jitter_buffer.late_useful_packets;
        out->underrun_ratio = s.jitter_buffer.underrun_ratio;
        out->fill_duty = s.jitter_buffer.fill_duty;
        out->drop_duty = s.jitter_buffer.drop_duty;
        out->lead_ms = s.jitter_buffer.lead_ms;
        // Buffer 决策层观测（末尾追加，与 C 结构体声明顺序一致）。
        out->jitter_control.adaptive = s.jitter_control.adaptive ? 1 : 0;
        out->jitter_control.desired_slots = s.jitter_control.desired_slots;
        out->jitter_control.min_slots = s.jitter_control.min_slots;
        out->jitter_control.max_slots = s.jitter_control.max_slots;
        out->jitter_control.margin_source = s.jitter_control.margin_source;
        out->jitter_control.path = s.jitter_control.path;
        out->jitter_control.floor_bound = s.jitter_control.floor_bound ? 1 : 0;
        out->jitter_control.cap_bound = s.jitter_control.cap_bound ? 1 : 0;
        out->jitter_control.underrun_penalty = s.jitter_control.underrun_penalty;
        out->jitter_control.dwell_remaining_ms = s.jitter_control.dwell_remaining_ms;
        out->jitter_control.fall_room_slots = s.jitter_control.fall_room_slots;
        out->jitter_control.stall_events = s.jitter_control.stall_events;
        out->jitter_control.stall_peak_ms = s.jitter_control.stall_peak_ms;
        out->jitter_control.last_stall_gap_ms = s.jitter_control.last_stall_gap_ms;
        out->jitter_control.arrival_interval_ms = s.jitter_control.arrival_interval_ms;
        out->jitter_control.band_warning_low = s.jitter_control.band_warning_low;
        out->jitter_control.band_normal_low = s.jitter_control.band_normal_low;
        out->jitter_control.band_normal_high = s.jitter_control.band_normal_high;
        out->jitter_control.band_warning_high = s.jitter_control.band_warning_high;
        out->jitter_control.conceal_run_slots = s.jitter_control.conceal_run_slots;
        out->jitter_control.underrun_run_slots = s.jitter_control.underrun_run_slots;
        // 切换事务序号（末尾追加）。
        out->switch_seq = s.switch_seq;
        return AQUA_OK;
    } catch (...) {
        aqua::log_error("capi: aqua_client_get_diagnostics aborted by C++ exception");
        return AQUA_ERR_INTERNAL;
    }
}

int aqua_client_set_playback_device(aqua_client_t* client, const char* device_id)
{
    try {
        if (client == nullptr || client->runtime == nullptr) {
            return AQUA_ERR_INVALID_ARGUMENT;
        }
        std::optional<aqua::audio::AudioDeviceId> target;
        if (device_id != nullptr && device_id[0] != '\0') {
            target = aqua::audio::AudioDeviceId(device_id);
        }
        aqua::log_info_fmt("capi: set_playback_device requested: {}",
            target ? target->value() : std::string("follow_system"));
        const auto result = client->runtime->set_playback_device(std::move(target));
        if (!result.has_value()) {
            if (result.error() == aqua::audio::AudioError::NotRunning) {
                return AQUA_ERR_NOT_CONNECTED;
            }
            if (result.error() == aqua::audio::AudioError::InvalidArgument) {
                return AQUA_ERR_INVALID_ARGUMENT;
            }
            // Fatal 终态拒绝 / 其他事务拒绝：事务本身已按链耗尽处理，
            // 细节经诊断 switch_outcome / switch_error 观察。
            return AQUA_ERR_SWITCH_FAILED;
        }
        return AQUA_OK;
    } catch (...) {
        aqua::log_error("capi: aqua_client_set_playback_device aborted by C++ exception");
        return AQUA_ERR_INTERNAL;
    }
}

void aqua_client_notify_devices_changed(aqua_client_t* client,
    const char* const* present_ids, int32_t count) noexcept
{
    if (client == nullptr || client->runtime == nullptr) {
        return;
    }
    try {
        // count 来自不可信调用方：钳制后转换，快照语义幂等可丢，非法直接丢弃。
        if (count < 0 || count > 4096) {
            return;
        }
        std::vector<aqua::audio::AudioDeviceId> ids;
        if (present_ids != nullptr && count > 0) {
            ids.reserve(static_cast<std::size_t>(count));
            for (int32_t i = 0; i < count; ++i) {
                if (present_ids[i] != nullptr && present_ids[i][0] != '\0') {
                    ids.emplace_back(present_ids[i]);
                }
            }
        }
        // 合并去抖与路由决策全部在 core（ioc 线程）；本函数只做边界转换。
        client->runtime->notify_devices_changed(std::move(ids));
    } catch (...) {
        // 异常不得越过 C 边界（Android 整进程崩）：丢弃本批快照即可。
    }
}

int aqua_client_get_connect_result(const aqua_client_t* client,
    aqua_connect_result_t* out)
{
    try {
        if (client == nullptr || client->runtime == nullptr || out == nullptr) {
            return AQUA_ERR_INVALID_ARGUMENT;
        }
        const auto& cr = client->runtime->connect_result();
        if (!cr.is_valid()) {
            return AQUA_ERR_NOT_CONNECTED;
        }
        out->session_id = cr.session_id;
        // 截断保护：地址缓冲 64 字节，含结尾 NUL。
        std::snprintf(out->advertised_udp_address, sizeof(out->advertised_udp_address), "%s", cr.advertised_udp_address.c_str());
        out->advertised_udp_port = cr.advertised_udp_port;
        out->audio_encoding = static_cast<std::int32_t>(cr.audio_format.encoding);
        out->channels = cr.audio_format.channels;
        out->sample_rate = cr.audio_format.sample_rate;
        out->frame_count = cr.frame_count;
        // 动态字段：当前学到的实际对端（HeartbeatAck 来源），建连时确定。
        // 未学到则留空。
        const auto learned = client->runtime->learned_peer_endpoint();
        if (learned) {
            std::snprintf(out->learned_udp_address, sizeof(out->learned_udp_address),
                "%s", learned->address().to_string().c_str());
            out->learned_udp_port = learned->port();
        } else {
            out->learned_udp_address[0] = '\0';
            out->learned_udp_port = 0;
        }
        return AQUA_OK;
    } catch (...) {
        aqua::log_error("capi: aqua_client_get_connect_result aborted by C++ exception");
        return AQUA_ERR_INTERNAL;
    }
}
