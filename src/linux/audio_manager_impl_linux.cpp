//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_linux.h"
#include "config.h"

#include <iostream>
#include <spdlog/spdlog.h>
#include <string>

#ifdef __linux__

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
// TODO: audio_manager
// #include "audio_manager.h"

void on_stream_state_changed_cb(void* userdata,
    enum pw_stream_state old,
    enum pw_stream_state state,
    const char* error);

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

// 静态回调函数
void on_process(void* userdata)
{
    auto* audio_manager = static_cast<audio_manager_impl*>(userdata);
    if (!audio_manager || !audio_manager->p_stream) {
        spdlog::error("Invalid impl or p_stream in on_process callback.");
        return;
    }

    std::lock_guard<std::mutex> lock(audio_manager->m_mutex);
    if (!audio_manager->m_is_capturing) {
        spdlog::trace("Not capturing. Skip on_process.");
        return;
    }

    struct pw_buffer* b = pw_stream_dequeue_buffer(audio_manager->p_stream);
    if (!b) {
        spdlog::warn("No available pw_buffer (out of buffers).");
        return;
    }

    auto* buf = b->buffer;
    if (!buf || !buf->datas[0].data) {
        spdlog::warn("Invalid buffer data in on_process.");
        pw_stream_queue_buffer(audio_manager->p_stream, b);
        return;
    }

    // 取得 samples
    const auto* samples = static_cast<const float*>(buf->datas[0].data);
    const uint32_t n_samples = buf->datas[0].chunk->size / sizeof(float);

    // 调用 audio_manager_impl 实例的方法
    // 这里使用的是span，因为在这个阶段还没有需要复制的地方，直接通过span读取原始数据，性能更好
    auto samples_span = std::span<const float>(samples, n_samples);
    audio_manager->process_audio_buffer(samples_span);

    // 处理完后记得将 buffer 返还
    pw_stream_queue_buffer(audio_manager->p_stream, b);
}

void on_quit(void* userdata, int /*signal_number*/)
{
    auto* impl = static_cast<audio_manager_impl*>(userdata);
    spdlog::info("Received quit signal, stop requested...");
}

void on_stream_process_cb(void* userdata)
{
    on_process(userdata);
}

void on_stream_state_changed_cb(void* userdata,
    const enum pw_stream_state old,
    const enum pw_stream_state state,
    const char* error)
{
    auto* impl = static_cast<audio_manager_impl*>(userdata);
    if (!impl) {
        return;
    }
    if (error && *error) {
        spdlog::error("Stream error: {}", error);
    }
    spdlog::info("Stream state changed from {} to {}",
        pw_stream_state_as_string(old),
        pw_stream_state_as_string(state));
}

// clang-format off
 constexpr struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,   // version
    nullptr,                    // destroy
    on_stream_state_changed_cb, // state_changed
    nullptr,                    // control_info
    nullptr,                    // io_changed
    nullptr,                    // param_changed
    nullptr,                    // add_buffer
    nullptr,                    // remove_buffer
    on_stream_process_cb,       // process
    nullptr,                    // drained
    nullptr,                    // command
    nullptr                     // trigger_done
};
// clang-format on

audio_manager_impl::audio_manager_impl()
    : p_params {}
    , m_buffer {}
    , m_builder()
{
    // 可在这里做更多初始化
    spdlog::debug("audio_manager_impl constructor.");
}

audio_manager_impl::~audio_manager_impl()
{
    // 停止采集线程
    if (m_capture_thread.joinable()) {
        m_capture_thread.request_stop();
        m_capture_thread.join();
    }

    // 销毁流
    if (p_stream) {
        spdlog::info("Destroying stream: {}", pw_stream_get_name(p_stream));
        pw_stream_destroy(p_stream);
        p_stream = nullptr;
    }

    // 销毁上下文
    if (p_context) {
        pw_context_destroy(p_context);
        p_context = nullptr;
    }

    // 销毁主循环
    if (p_main_loop) {
        pw_main_loop_destroy(p_main_loop);
        p_main_loop = nullptr;
    }

    // PipeWire 去初始化
    pw_deinit();
    spdlog::info("audio_manager_impl destroyed.");
}

bool audio_manager_impl::init()
{
    pw_init(nullptr, nullptr);
    log_pipewire_debug_info();

    p_main_loop = pw_main_loop_new(nullptr);
    if (!p_main_loop) {
        spdlog::error("Failed to create PipeWire main loop.");
        return false;
    }

    spdlog::info("Audio manager init() complete.");
    return true;
}

bool audio_manager_impl::setup_stream()
{
    if (!p_main_loop) {
        spdlog::error("setup_stream() failed: p_main_loop is null.");
        return false;
    }

    // 构建一些节点属性
    const std::string latency_str = std::to_string(m_stream_config.latency) + "/" + std::to_string(m_stream_config.rate);
    const std::string rate_str = "1/" + std::to_string(m_stream_config.rate);

    // 名称可随需求进行改变
    const std::string stream_name = std::string(aqua_core_BINARY_NAME) + " capture";

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_NODE_NAME, (std::string(aqua_core_BINARY_NAME) + " capture").c_str(),
        PW_KEY_NODE_DESCRIPTION, (std::string(aqua_core_BINARY_NAME) + " Audio Capture").c_str(),
        PW_KEY_NODE_LATENCY, latency_str.c_str(),
        PW_KEY_NODE_RATE, rate_str.c_str(),
        nullptr);

    spdlog::debug("Creating stream with rate={}Hz, channels={}",
        m_stream_config.rate, m_stream_config.channels);

    p_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(p_main_loop), // loop
        stream_name.c_str(), // name
        props, // properties
        &stream_events, // events
        this // userdata
    );

    if (!p_stream) {
        spdlog::error("Failed to create PipeWire stream.");
        return false;
    }

    // 准备音频格式信息
    m_builder = SPA_POD_BUILDER_INIT(m_buffer, sizeof(m_buffer));

    spa_audio_info_raw info {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = m_stream_config.rate,
        .channels = static_cast<uint32_t>(m_stream_config.channels)
    };

    p_params[0] = spa_format_audio_raw_build(&m_builder, SPA_PARAM_EnumFormat, &info);

    // 将流连接到 PipeWire
    int ret = pw_stream_connect(
        p_stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        p_params,
        1);

    if (ret < 0) {
        spdlog::error("Failed to connect stream, error code: {}", ret);
        return false;
    }

    spdlog::info("Stream connected: {}", pw_stream_get_name(p_stream));
    return true;
}

bool audio_manager_impl::start_capture(AudioDataCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_capturing) {
        spdlog::warn("Already capturing. start_capture() ignored.");
        return false;
    }

    // 设置数据回调
    m_data_callback = std::move(callback);
    m_promise_initialized = std::promise<void>();

    // 启动采集线程
    m_capture_thread = std::jthread([this]() {
        m_is_capturing = true;
        m_promise_initialized.set_value();

        if (p_main_loop) {
            // 运行循环，阻塞直到 pw_main_loop_quit() 被调用
            pw_main_loop_run(p_main_loop);
        }

        m_is_capturing = false;
    });

    auto future = m_promise_initialized.get_future();
    future.wait(); // 等待子线程进入采集循环
    return true;
}

bool audio_manager_impl::stop_capture()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_capturing) {
        spdlog::warn("Not capturing. stop_capture() ignored.");
        return false;
    }

    // 让 main_loop 退出 pw_main_loop_run() 阻塞
    if (p_main_loop) {
        pw_main_loop_quit(p_main_loop);
    }

    // 等线程退出
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }
    return true;
}

bool audio_manager_impl::is_capturing() const
{
    return m_is_capturing.load();
}

const audio_manager_impl::stream_config& audio_manager_impl::get_format() const
{
    return m_stream_config;
}

void audio_manager_impl::process_audio_buffer(const std::span<const float> audio_buffer)
{
    if (audio_buffer.empty()) {
        return;
    }

    if (m_data_callback) {
        std::vector<float> samples(audio_buffer.begin(), audio_buffer.end());
        m_data_callback(samples);
    } else {
        spdlog::warn("No callback set. Skip process_audio_buffer().");
    }
}

#endif // __linux__
