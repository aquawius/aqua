//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_linux.h"
#include "config.h"

#include <spdlog/spdlog.h>
#include <spa/utils/result.h>

#ifdef __linux__

// PipeWire回调声明
void on_process(void* userdata);
void on_stream_process_cb(void* userdata);
void on_stream_state_changed_cb(void* userdata, pw_stream_state, pw_stream_state, const char*);

static constexpr struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed_cb,
    .process = on_stream_process_cb
};

audio_manager_impl::audio_manager_impl() {
    spdlog::debug("[Linux] Audio manager instance created.");
}

audio_manager_impl::~audio_manager_impl() {
    stop_capture(); // 确保先停止捕获

    // 按顺序释放资源：流 → 上下文 → 主循环
    if (p_stream) {
        spdlog::debug("Destroying PipeWire stream...");
        pw_stream_destroy(p_stream);
        p_stream = nullptr;
    }

    if (p_context) {
        spdlog::debug("Destroying PipeWire context...");
        pw_context_destroy(p_context);
        p_context = nullptr;
    }

    if (p_main_loop) {
        spdlog::debug("Destroying PipeWire main loop...");
        pw_main_loop_destroy(p_main_loop);
        p_main_loop = nullptr;
    }

    pw_deinit();
    spdlog::info("[Linux] Audio manager destroyed.");
}

void log_pipewire_debug_info()
{
    spdlog::debug("PipeWire Header Version: {}",
        pw_get_client_name() ? pw_get_client_name() : "<unknown>");
    spdlog::debug("PipeWire Library Version: {}",
        pw_get_library_version() ? pw_get_library_version() : "<unknown>");
    spdlog::debug("Application Name: {}",
        pw_get_application_name() ? pw_get_application_name() : "<unknown>");
    spdlog::debug("Program Name: {}",
        pw_get_prgname() ? pw_get_prgname() : "<unknown>");
    spdlog::debug("User Name: {}",
        pw_get_user_name() ? pw_get_user_name() : "<unknown>");
    spdlog::debug("Host Name: {}",
        pw_get_host_name() ? pw_get_host_name() : "<unknown>");
    spdlog::debug("Client Name: {}",
        pw_get_client_name() ? pw_get_client_name() : "<unknown>");
}

// 初始化
bool audio_manager_impl::init() {
    pw_init(nullptr, nullptr);
    spdlog::info("[Linux] PipeWire initialized (version: {})", pw_get_library_version());

    p_main_loop = pw_main_loop_new(nullptr);
    if (!p_main_loop) {
        spdlog::critical("[Linux] Failed to create PipeWire main loop.");
        return false;
    }

    // 创建上下文
    p_context = pw_context_new(pw_main_loop_get_loop(p_main_loop), nullptr, 0);
    if (!p_context) {
        spdlog::critical("[Linux] Failed to create PipeWire context.");
        return false;
    }

    return true;
}

// 流配置
bool audio_manager_impl::setup_stream() {
    if (!p_main_loop || !p_context) {
        spdlog::error("[Linux] setup_stream() failed: PipeWire not initialized.");
        return false;
    }

    // 配置流参数
    const std::string latency_str = std::to_string(m_stream_config.latency) + "/" +
                                   std::to_string(m_stream_config.rate);
    const std::string stream_name = std::string(aqua_core_BINARY_NAME) + "-capture";

    // 创建流属性（优化日志和参数）
    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true",   // 捕获扬声器输出
        PW_KEY_NODE_NAME, stream_name.c_str(),
        PW_KEY_NODE_LATENCY, latency_str.c_str(),
        nullptr);


    // 创建流
    p_stream = pw_stream_new_simple(
           pw_main_loop_get_loop(p_main_loop), // loop
           stream_name.c_str(), // name
           props, // properties
           &stream_events, // events
           this // userdata
       );

    if (!p_stream) {
        spdlog::error("[Linux] Failed to create PipeWire stream.");
        return false;
    }

    // 配置音频格式
    m_builder = SPA_POD_BUILDER_INIT(m_buffer, sizeof(m_buffer));
    spa_audio_info_raw audio_info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = m_stream_config.rate,
        .channels = m_stream_config.channels
    };
    p_params[0] = spa_format_audio_raw_build(&m_builder, SPA_PARAM_EnumFormat, &audio_info);

    // 连接流
    int ret = pw_stream_connect(
        p_stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_RT_PROCESS),
        p_params,
        1);

    if (ret < 0) {
        spdlog::error("[Linux] Stream connection failed: {} (code: {})", spa_strerror(ret), ret);
        return false;
    }

    spdlog::info("[Linux] Stream configured: {} Hz, {} channels",
                m_stream_config.rate, m_stream_config.channels);
    return true;
}

// 捕获控制
bool audio_manager_impl::start_capture(AudioDataCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_capturing) {
        spdlog::warn("[Linux] Capture is already running.");
        return false;
    }

    m_data_callback = std::move(callback);
    m_promise_initialized = std::promise<void>();

    // 启动捕获线程（使用stop_token确保安全退出）
    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_capturing = true;
        m_promise_initialized.set_value();

        // 设置停止回调
        std::stop_callback stop_cb(stop_token, [this]() {
            if (p_main_loop) pw_main_loop_quit(p_main_loop);
        });

        // 运行主循环
        spdlog::info("[Linux] Starting PipeWire main loop...");
        pw_main_loop_run(p_main_loop);
        spdlog::info("[Linux] PipeWire main loop exited.");

        m_is_capturing = false;
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

bool audio_manager_impl::stop_capture() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_capturing) {
        spdlog::warn("[Linux] No active capture to stop.");
        return false;
    }

    // 请求线程停止
    m_capture_thread.request_stop();
    if (p_main_loop) pw_main_loop_quit(p_main_loop);

    // 等待线程退出
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
        spdlog::debug("[Linux] Capture thread joined.");
    }

    return true;
}

// 数据处理
void audio_manager_impl::process_audio_buffer(const std::span<const float> audio_buffer) const
{
    if (audio_buffer.empty()) return;

    if (m_data_callback) {
        m_data_callback(audio_buffer);
        // spdlog::trace("[Linux] Sent {} samples to callback.", data.size());
    } else {
        spdlog::warn("[Linux] No callback set.");
    }
}

// PipeWire回调函数
void on_process(void* userdata) {
    auto* mgr = static_cast<audio_manager_impl*>(userdata);
    if (!mgr || !mgr->p_stream) {
        spdlog::warn("[Linux] Invalid manager or stream in callback.");
        return;
    }

    std::lock_guard<std::mutex> lock(mgr->m_mutex);
    if (!mgr->m_is_capturing) return;

    struct pw_buffer* buffer = pw_stream_dequeue_buffer(mgr->p_stream);
    if (!buffer || !buffer->buffer || !buffer->buffer->datas[0].data) {
        spdlog::warn("[Linux] Invalid buffer received.");
        return;
    }

    // 直接传递数据span，避免拷贝
    const auto* data = static_cast<const float*>(buffer->buffer->datas[0].data);
    const uint32_t n_samples = buffer->buffer->datas[0].chunk->size / sizeof(float);
    mgr->process_audio_buffer(std::span<const float>(data, n_samples));

    pw_stream_queue_buffer(mgr->p_stream, buffer);
}

void on_stream_state_changed_cb(void* userdata, const pw_stream_state old, const pw_stream_state state, const char* error) {
    auto* audio_manager = static_cast<audio_manager_impl*>(userdata);
    if (error) spdlog::error("[Linux] Stream error: {}", error);
    spdlog::info("[Linux] Stream state changed: {} -> {}",
                pw_stream_state_as_string(old),
                pw_stream_state_as_string(state));
}

void on_stream_process_cb(void* userdata) {
    on_process(userdata);
}

#endif // __linux__