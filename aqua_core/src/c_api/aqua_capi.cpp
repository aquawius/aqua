// Aqua client C API 实现：ClientRuntime 的薄 wrapper + 监督线程。
// 业务、生命周期、音频路径全部在 aqua::runtime::ClientRuntime；
// 本文件只负责 C 边界转换与 CLI control timer 语义的线程化。

#include "aqua/c_api/aqua_capi.h"

#include "aqua/logger/logger.h"
#include "aqua/runtime/client_runtime.h"
#include "aqua/version.h"

#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

// ---- 枚举镜像数值与 core C++ 枚举强一致（编译期契约，全值锁定）----
#define AQUA_CAPI_ASSERT_ENUM_MIRROR(cpp_enum, c_value) \
    static_assert(static_cast<int>(cpp_enum) == c_value, #cpp_enum " != " #c_value)

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Created, AQUA_STATE_CREATED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Starting, AQUA_STATE_STARTING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Running, AQUA_STATE_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Degraded, AQUA_STATE_DEGRADED);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Stopping, AQUA_STATE_STOPPING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::RuntimeState::Stopped, AQUA_STATE_STOPPED);

AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::PlaybackState::Inactive, AQUA_PLAYBACK_INACTIVE);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::PlaybackState::Starting, AQUA_PLAYBACK_STARTING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::PlaybackState::Running, AQUA_PLAYBACK_RUNNING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::PlaybackState::Switching, AQUA_PLAYBACK_SWITCHING);
AQUA_CAPI_ASSERT_ENUM_MIRROR(aqua::runtime::PlaybackState::Fatal, AQUA_PLAYBACK_FATAL);

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
    std::thread io_thread;

    explicit aqua_client(aqua::runtime::ClientRuntimeConfig cfg)
        : config(std::move(cfg))
    {
    }

    // CLI control timer 的等价物：500ms 监督 tick，Degraded / hello_failed -> stop。
    // 运行在 io_thread 上（唯一 ioc.run() 调用者）。
    void supervision_main()
    {
        auto timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> tick;
        tick = [this, timer, &tick](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            const auto snapshot = runtime->take_diagnostics_snapshot();
            if (snapshot.state == aqua::runtime::RuntimeState::Degraded
                || snapshot.net.hello_failed) {
                aqua::log_debug_fmt("capi: supervision observed terminal condition: state={} hello_failed={}",
                    aqua::runtime::runtime_state_name(snapshot.state), snapshot.net.hello_failed);
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
    using aqua::runtime::RuntimeState;
    switch (static_cast<RuntimeState>(state)) {
    case RuntimeState::Created:
        return "created";
    case RuntimeState::Starting:
        return "starting";
    case RuntimeState::Running:
        return "running";
    case RuntimeState::Degraded:
        return "degraded";
    case RuntimeState::Stopping:
        return "stopping";
    case RuntimeState::Stopped:
        return "stopped";
    default:
        return "unknown";
    }
}

const char* aqua_audio_error_name(int error)
{
    using aqua::audio::AudioError;
    switch (static_cast<AudioError>(error)) {
    case AudioError::None:
        return "none";
    case AudioError::DeviceNotFound:
        return "device_not_found";
    case AudioError::DeviceUnavailable:
        return "device_unavailable";
    case AudioError::DeviceDisconnected:
        return "device_disconnected";
    case AudioError::FormatUnsupported:
        return "format_unsupported";
    case AudioError::NotSupported:
        return "not_supported";
    case AudioError::PermissionDenied:
        return "permission_denied";
    case AudioError::AlreadyRunning:
        return "already_running";
    case AudioError::NotRunning:
        return "not_running";
    case AudioError::InvalidArgument:
        return "invalid_argument";
    case AudioError::BackendFailed:
        return "backend_failed";
    default:
        return "unknown";
    }
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
    if (config->jitter_buffer_slots != 0) {
        cfg.jitter_buffer_slots = config->jitter_buffer_slots;
    }
    if (config->hello_interval_ms != 0) {
        cfg.hello_interval = std::chrono::milliseconds(config->hello_interval_ms);
    }
    if (config->playback_frames_per_buffer != 0) {
        cfg.playback.frames_per_buffer = config->playback_frames_per_buffer;
    }
    if (config->force_udp_port != 0) {
        cfg.force_udp_port = config->force_udp_port;
    }
    cfg.playback.low_latency = config->playback_low_latency != 0;

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
    if (client == nullptr || client->runtime == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    if (!client->runtime->start()) {
        aqua::log_debug("capi: client start failed; handle is now Stopped, destroy to retry");
        return AQUA_ERR_START_FAILED;
    }

    try {
        client->io_thread = std::thread([client] { client->supervision_main(); });
    } catch (...) {
        // 线程创建失败：runtime 已 Running 但无人驱动 io_context（HELLO 定时器
        // 不会走）。按失败处理，回滚到 Stopped。
        client->runtime->stop();
        return AQUA_ERR_START_FAILED;
    }
    return AQUA_OK;
}

int aqua_client_stop(aqua_client_t* client)
{
    if (client == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    client->runtime->stop();
    client->ioc.stop();
    if (client->io_thread.joinable()) {
        client->io_thread.join();
    }
    return AQUA_OK;
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

int aqua_client_get_diagnostics(const aqua_client_t* client,
    aqua_client_diagnostics_t* out)
{
    if (client == nullptr || client->runtime == nullptr || out == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    const auto s = client->runtime->take_diagnostics_snapshot();
    out->state = static_cast<int32_t>(s.state);
    out->last_audio_error = static_cast<int32_t>(s.last_audio_error);
    out->playback_running = s.playback_running ? 1 : 0;
    out->playback_state = static_cast<int32_t>(s.playback_state);

    out->net.rx_packets = s.net.transport.rx_packets;
    out->net.rx_bytes = s.net.transport.rx_bytes;
    out->net.rx_errors = s.net.transport.rx_errors;
    out->net.tx_packets = s.net.transport.tx_packets;
    out->net.tx_bytes = s.net.transport.tx_bytes;
    out->net.tx_errors = s.net.transport.tx_errors;
    out->net.tx_dropped = s.net.transport.tx_dropped;
    out->net.tx_enqueue_failures = s.net.transport.tx_enqueue_failures;
    out->net.tx_queue_depth = s.net.transport.tx_queue_depth;
    out->net.hello_ack_count = s.net.hello_ack_count;
    out->net.hello_ack_misses = s.net.hello_ack_misses;
    out->net.hello_ack_age_ms = s.net.hello_ack_age_ms;
    out->net.hello_send_attempts = s.net.hello_send_attempts;
    out->net.hello_ack_miss_events = s.net.hello_ack_miss_events;
    out->net.audio_frames_accepted = s.net.audio_frames_accepted;
    out->net.malformed_datagrams = s.net.malformed_datagrams;
    out->net.unexpected_sender_datagrams = s.net.unexpected_sender_datagrams;
    out->net.wrong_session_acks = s.net.wrong_session_acks;
    out->net.audio_payload_mismatches = s.net.audio_payload_mismatches;
    out->net.non_audio_datagrams = s.net.non_audio_datagrams;
    out->net.hello_failed = s.net.hello_failed ? 1 : 0;

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

    out->playback.pull_calls = s.playback.pull_calls;
    out->playback.pull_frames = s.playback.pull_frames;
    out->playback.pull_silence_frames = s.playback.pull_silence_frames;

    out->stream.backend = static_cast<uint32_t>(s.stream.backend);
    out->stream.sample_rate = s.stream.sample_rate;
    out->stream.channels = s.stream.channels;
    out->stream.performance_mode = s.stream.performance_mode;
    out->stream.frames_per_burst = s.stream.frames_per_burst;
    out->stream.buffer_capacity_frames = s.stream.buffer_capacity_frames;
    return AQUA_OK;
}

int aqua_client_get_connect_result(const aqua_client_t* client,
    aqua_connect_result_t* out)
{
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
    // 动态字段：当前学到的实际对端（HELLO_ACK 来源），每次有效 HELLO_ACK 刷新为 sender，
    // 不是一次性初始化参数。未学到则留空。
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
}
