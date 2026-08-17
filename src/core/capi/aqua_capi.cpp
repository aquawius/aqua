// Aqua C API 实现：把 aqua.h 的 C ABI 桥接到 aqua::client::ClientRuntime /
// aqua::server::ServerRuntime。头文件不含任何 STL / Asio / gRPC 类型，本文件
// 是唯一接触运行时的 C++ 翻译层。
//
// 约定：
//   - 每个 C 函数都用 try/catch 全捕获，转成负返回码，跨边界绝不抛异常。
//   - 字符串入参在 start() 时拷贝进 std::string，出参指针在句柄内缓存、句柄销毁前有效。
//   - 回调 struct 与 user_data 在 start() 时按值拷贝，调用方 struct 可即时释放。

#include "aqua.h"

#include "core/client/client_runtime.h"
#include "core/diagnostics/diagnostics_manager.h"
#include "core/logger/logger.h"
#include "core/public/audio_format.h"
#include "core/public/version.h"
#include "core/server/server_runtime.h"

#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

// ---- 编译期同步校验：aqua.h 的枚举值必须与 core 内部 AudioEncoding 一致 ----
static_assert(static_cast<int>(aqua::AudioEncoding::Invalid) == AQUA_ENCODING_INVALID,
              "AQUA_ENCODING_INVALID 与 AudioEncoding::Invalid 不同步");
static_assert(static_cast<int>(aqua::AudioEncoding::PcmS16LE) == AQUA_ENCODING_PCM_S16LE,
              "AQUA_ENCODING_PCM_S16LE 与 AudioEncoding::PcmS16LE 不同步");
static_assert(static_cast<int>(aqua::AudioEncoding::PcmS32LE) == AQUA_ENCODING_PCM_S32LE,
              "AQUA_ENCODING_PCM_S32LE 与 AudioEncoding::PcmS32LE 不同步");
static_assert(static_cast<int>(aqua::AudioEncoding::PcmF32LE) == AQUA_ENCODING_PCM_F32LE,
              "AQUA_ENCODING_PCM_F32LE 与 AudioEncoding::PcmF32LE 不同步");
static_assert(static_cast<int>(aqua::AudioEncoding::PcmS24LE) == AQUA_ENCODING_PCM_S24LE,
              "AQUA_ENCODING_PCM_S24LE 与 AudioEncoding::PcmS24LE 不同步");
static_assert(static_cast<int>(aqua::AudioEncoding::PcmU8) == AQUA_ENCODING_PCM_U8,
              "AQUA_ENCODING_PCM_U8 与 AudioEncoding::PcmU8 不同步");

// ---- C ↔ C++ 类型转换 ----

aqua_audio_format_t to_c_format(const aqua::AudioFormat& format) noexcept
{
    return aqua_audio_format_t {
        static_cast<int32_t>(format.encoding),
        format.channels,
        format.sample_rate,
    };
}

aqua_client_state_t to_c_state(aqua::client::ClientState state) noexcept
{
    switch (state) {
    case aqua::client::ClientState::Idle:
        return AQUA_CLIENT_IDLE;
    case aqua::client::ClientState::Connecting:
        return AQUA_CLIENT_CONNECTING;
    case aqua::client::ClientState::Playing:
        return AQUA_CLIENT_PLAYING;
    case aqua::client::ClientState::Reconnecting:
        return AQUA_CLIENT_RECONNECTING;
    case aqua::client::ClientState::Stopped:
        return AQUA_CLIENT_STOPPED;
    case aqua::client::ClientState::Failed:
        return AQUA_CLIENT_FAILED;
    }
    return AQUA_CLIENT_IDLE;
}

// 把 C 回调 struct 桥接为 C++ 回调。函数指针与 user_data 按值拷贝，
// 因此调用方的 aqua_client_callbacks_t 在 start() 返回后可安全释放。
aqua::client::ClientCallbacks to_cpp_callbacks(const aqua_client_callbacks_t* cb)
{
    aqua::client::ClientCallbacks out;
    if (cb == nullptr) {
        return out;
    }
    void* const ud = cb->user_data;
    const auto state_fn = cb->on_state_change;
    const auto format_fn = cb->on_format;
    const auto error_fn = cb->on_error;
    const auto stopped_fn = cb->on_stopped;

    out.on_state_change = [ud, state_fn](aqua::client::ClientState s) {
        if (state_fn) {
            state_fn(ud, to_c_state(s));
        }
    };
    out.on_format = [ud, format_fn](const aqua::AudioFormat& format) {
        if (format_fn) {
            const auto c_format = to_c_format(format);
            format_fn(ud, &c_format);
        }
    };
    out.on_error = [ud, error_fn](std::string message) {
        if (error_fn) {
            error_fn(ud, message.c_str());
        }
    };
    out.on_stopped = [ud, stopped_fn]() {
        if (stopped_fn) {
            stopped_fn(ud);
        }
    };
    return out;
}

aqua::server::ServerCallbacks to_cpp_callbacks(const aqua_server_callbacks_t* cb)
{
    aqua::server::ServerCallbacks out;
    if (cb == nullptr) {
        return out;
    }
    void* const ud = cb->user_data;
    const auto started_fn = cb->on_started;
    const auto error_fn = cb->on_error;
    const auto stopped_fn = cb->on_stopped;

    out.on_started = [ud, started_fn]() {
        if (started_fn) {
            started_fn(ud);
        }
    };
    out.on_error = [ud, error_fn](std::string message) {
        if (error_fn) {
            error_fn(ud, message.c_str());
        }
    };
    out.on_stopped = [ud, stopped_fn]() {
        if (stopped_fn) {
            stopped_fn(ud);
        }
    };
    return out;
}

// C++ 诊断快照 -> C 结构按值拷贝。字段一一对应 DiagnosticsManager::Snapshot。
void to_c_diagnostics(const aqua::diag::DiagnosticsManager::Snapshot& s,
                      aqua_diagnostics_t* out) noexcept
{
    out->rtt_ms = s.rtt_ms;
    out->interarrival_jitter_ms = s.interarrival_jitter_ms;
    out->packets_received = s.packets_received;
    out->packets_lost = s.packets_lost;
    out->duplicates = s.duplicates;
    out->late_packets = s.late_packets;
    out->recv_audio_bytes = s.recv_audio_bytes;
    out->recv_hello_acks = s.recv_hello_acks;

    out->jb_current_packets = s.jb_current_packets;
    out->jb_current_ms = s.jb_current_ms;
    out->jb_avg_ms = s.jb_avg_ms;
    out->jb_min_ms = s.jb_min_ms;
    out->jb_max_ms = s.jb_max_ms;
    out->jb_capacity_ms = s.jb_capacity_ms;

    out->rb_current_ms = s.rb_current_ms;
    out->rb_avg_ms = s.rb_avg_ms;
    out->rb_min_ms = s.rb_min_ms;
    out->rb_max_ms = s.rb_max_ms;
    out->rb_capacity_ms = s.rb_capacity_ms;
    out->underruns = s.underruns;
    out->deadline_misses = s.deadline_misses;

    out->short_slope_samples_per_s = s.short_slope_samples_per_s;
    out->long_slope_samples_per_s = s.long_slope_samples_per_s;

    out->end_to_end_ms = s.end_to_end_ms;
    out->drift_ppm = s.drift_ppm;

    // v2 追加字段（老调用方 memset(0) 初始化时保持 0 语义安全）
    out->jb_target_ms = s.jb_target_ms;
    out->rb_rearms = s.rb_rearms;
}

} // namespace

// ---- 句柄：持有运行时 + 后台 run() 线程 + 错误缓存 ----
// aqua.h 中声明为不透明 `typedef struct aqua_client aqua_client_t;`，
// 此处给出完整定义（C++ 侧私有，不泄漏到 C）。
struct aqua_client {
    aqua::client::ClientRuntime runtime;
    std::thread worker; // 运行 runtime.run()，监控直到关闭并 join 会话线程
    mutable std::mutex err_mutex;
    mutable std::string last_error;
    bool started = false;
};

struct aqua_server {
    aqua::server::ServerRuntime runtime;
    std::thread worker; // 运行 runtime.run()（健康监控 + 收尾）
    mutable std::mutex err_mutex;
    mutable std::string last_error;
    bool started = false;
};

extern "C" {

const char* aqua_version(void)
{
    return AQUA_CORE_VERSION;
}

int aqua_set_log_level(aqua_log_level_t level)
{
    try {
        switch (level) {
        case AQUA_LOG_TRACE:
            aqua::set_log_level(aqua::LogLevel::Trace);
            return AQUA_OK;
        case AQUA_LOG_DEBUG:
            aqua::set_log_level(aqua::LogLevel::Debug);
            return AQUA_OK;
        case AQUA_LOG_INFO:
            aqua::set_log_level(aqua::LogLevel::Info);
            return AQUA_OK;
        case AQUA_LOG_WARN:
            aqua::set_log_level(aqua::LogLevel::Warn);
            return AQUA_OK;
        case AQUA_LOG_ERROR:
            aqua::set_log_level(aqua::LogLevel::Error);
            return AQUA_OK;
        }
        return AQUA_ERR_INVALID_ARGUMENT;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

void aqua_client_config_init(aqua_client_config_t* config)
{
    if (config == nullptr) {
        return;
    }
    *config = aqua_client_config_t {};
    config->server_ip = "127.0.0.1";
    config->server_rpc_port = 50051;
    // jitter_buffer_ms 清零即默认语义（30ms），无需显式赋值
    config->playback_ringbuffer_size = 0;    // 0 = 默认 16KB
    config->auto_reconnect = 0;
    config->client_name = "aqua_client";
}

void aqua_server_config_init(aqua_server_config_t* config)
{
    if (config == nullptr) {
        return;
    }
    *config = aqua_server_config_t {};
    config->bind_ip = "0.0.0.0";
    config->rpc_port = 50051;
    config->udp_port = 50000;
    config->capture_ringbuffer_size = 0; // 0 = 默认 8KB
}

aqua_client_t* aqua_client_create(void)
{
    try {
        return new aqua_client {};
    } catch (...) {
        return nullptr;
    }
}

void aqua_client_destroy(aqua_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    try {
        client->runtime.shutdown();
        if (client->worker.joinable()) {
            client->worker.join();
        }
    } catch (...) {
        // 析构路径不抛异常
    }
    delete client;
}

int aqua_client_start(aqua_client_t* client,
                      const aqua_client_config_t* config,
                      const aqua_client_callbacks_t* callbacks)
{
    if (client == nullptr || config == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        if (client->started) {
            return AQUA_ERR_ALREADY_RUNNING;
        }

        aqua::client::ClientConfig cfg;
        cfg.server_ip = config->server_ip ? config->server_ip : "127.0.0.1";
        cfg.server_rpc_port = config->server_rpc_port ? config->server_rpc_port : 50051;
        cfg.auto_reconnect = config->auto_reconnect != 0;
        cfg.client_name = config->client_name ? config->client_name : "aqua_client";
        // v3 字段：0 = 默认语义（30ms），> 0 才覆盖。
        if (config->jitter_buffer_ms > 0) {
            cfg.runtime.jitter_buffer_ms = config->jitter_buffer_ms;
        }
        // v4 字段：0 = 默认语义（500 包），> 0 才覆盖。
        if (config->jitter_detect_window_packets > 0) {
            cfg.runtime.jitter_detect_window_packets = config->jitter_detect_window_packets;
        }
        if (config->playback_ringbuffer_size > 0) {
            cfg.runtime.playback_ringbuffer_size = config->playback_ringbuffer_size;
        }

        if (!client->runtime.start(cfg, to_cpp_callbacks(callbacks))) {
            return AQUA_ERR_ALREADY_RUNNING;
        }

        client->started = true;
        client->worker = std::thread([client]() {
            client->runtime.run();
        });
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

int aqua_client_shutdown(aqua_client_t* client)
{
    if (client == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        client->runtime.shutdown();
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

aqua_client_state_t aqua_client_state(const aqua_client_t* client)
{
    if (client == nullptr) {
        return AQUA_CLIENT_IDLE;
    }
    try {
        return to_c_state(client->runtime.state());
    } catch (...) {
        return AQUA_CLIENT_FAILED;
    }
}

int aqua_client_is_running(const aqua_client_t* client)
{
    if (client == nullptr) {
        return 0;
    }
    try {
        return client->runtime.is_running() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

const char* aqua_client_last_error(const aqua_client_t* client)
{
    if (client == nullptr) {
        return "";
    }
    try {
        std::lock_guard<std::mutex> lock(client->err_mutex);
        client->last_error = client->runtime.last_error();
        return client->last_error.c_str();
    } catch (...) {
        return "";
    }
}

int aqua_client_get_diagnostics(const aqua_client_t* client, aqua_diagnostics_t* out)
{
    if (client == nullptr || out == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        auto snap = client->runtime.diagnostics();
        if (!snap) {
            return AQUA_ERR_NOT_AVAILABLE;
        }
        to_c_diagnostics(*snap, out);
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

int aqua_client_get_audio_format(const aqua_client_t* client, aqua_audio_format_t* out)
{
    if (client == nullptr || out == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        auto format = client->runtime.audio_format();
        if (!format) {
            return AQUA_ERR_NOT_AVAILABLE;
        }
        *out = to_c_format(*format);
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

aqua_server_t* aqua_server_create(void)
{
    try {
        return new aqua_server {};
    } catch (...) {
        return nullptr;
    }
}

void aqua_server_destroy(aqua_server_t* server)
{
    if (server == nullptr) {
        return;
    }
    try {
        server->runtime.shutdown();
        if (server->worker.joinable()) {
            server->worker.join();
        }
    } catch (...) {
        // 析构路径不抛异常
    }
    delete server;
}

int aqua_server_start(aqua_server_t* server,
                      const aqua_server_config_t* config,
                      const aqua_server_callbacks_t* callbacks)
{
    if (server == nullptr || config == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        if (server->started) {
            return AQUA_ERR_ALREADY_RUNNING;
        }

        aqua::server::ServerConfig cfg;
        cfg.bind_ip = config->bind_ip ? config->bind_ip : "0.0.0.0";
        cfg.rpc_port = config->rpc_port ? config->rpc_port : 50051;
        cfg.udp_port = config->udp_port ? config->udp_port : 50000;
        if (config->capture_ringbuffer_size > 0) {
            cfg.runtime.capture_ringbuffer_size = config->capture_ringbuffer_size;
        }

        if (!server->runtime.start(cfg, to_cpp_callbacks(callbacks))) {
            return AQUA_ERR_START_FAILED; // 详情经 last_error 返回
        }

        server->started = true;
        server->worker = std::thread([server]() {
            server->runtime.run();
        });
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

int aqua_server_shutdown(aqua_server_t* server)
{
    if (server == nullptr) {
        return AQUA_ERR_INVALID_ARGUMENT;
    }
    try {
        server->runtime.shutdown();
        return AQUA_OK;
    } catch (...) {
        return AQUA_ERR_INTERNAL;
    }
}

int aqua_server_is_running(const aqua_server_t* server)
{
    if (server == nullptr) {
        return 0;
    }
    try {
        return server->runtime.is_running() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

const char* aqua_server_last_error(const aqua_server_t* server)
{
    if (server == nullptr) {
        return "";
    }
    try {
        std::lock_guard<std::mutex> lock(server->err_mutex);
        server->last_error = server->runtime.last_error();
        return server->last_error.c_str();
    } catch (...) {
        return "";
    }
}

} // extern "C"
