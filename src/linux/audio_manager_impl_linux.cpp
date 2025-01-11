//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_linux.h"
#include "audio_manager_impl_linux.h"
#include "audio_manager_impl_linux.h"

#include <config.h>
#include <iostream>
#include <spdlog/spdlog.h>
#ifdef __linux__

// #include "audio_manager.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <string>

void log_pipewire_debug_info()
{
    const char* header_version = pw_get_client_name();
    const char* library_version = pw_get_library_version();
    const char* app_name = pw_get_application_name();
    // TODO: Why app_name is null?
    const char* prg_name = pw_get_prgname();
    const char* user_name = pw_get_user_name();
    const char* host_name = pw_get_host_name();
    const char* client_name = pw_get_client_name();

    spdlog::debug("Header version: {}", header_version ? header_version : "<unknown>");
    spdlog::debug("Library version: {}", library_version ? library_version : "<unknown>");
    spdlog::debug("Application Name: {}", app_name ? app_name : "<unknown>");
    spdlog::debug("Program Name: {}", prg_name ? prg_name : "<unknown>");
    spdlog::debug("User Name: {}", user_name ? user_name : "<unknown>");
    spdlog::debug("Host Name: {}", host_name ? host_name : "<unknown>");
    spdlog::debug("Client Name: {}", client_name ? client_name : "<unknown>");
}

void on_quit(void* userdata, int signal_number)
{
    spdlog::info("Quitting...");
    // may use reinterpret_cast
    // pw_main_loop_quit();
}

void on_stream_process_cb(void* userdata)
{
    audio_manager_impl::on_process(userdata);
}

void on_stream_state_changed_cb(void* userdata,
    enum pw_stream_state old,
    enum pw_stream_state state,
    const char* error)
{
    if (error) {
        spdlog::error("Stream error: {}", error);
    }
    spdlog::info("Stream state changed from {} to {}",
        pw_stream_state_as_string(old),
        pw_stream_state_as_string(state));
}

// clang-format off
const struct pw_stream_events stream_events = {
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

void audio_manager_impl::on_process(void* userdata)
{
    auto* impl = static_cast<audio_manager_impl*>(userdata);
    struct pw_buffer* b;
    struct spa_buffer* buf;

    if ((b = pw_stream_dequeue_buffer(impl->p_stream)) == nullptr) {
        spdlog::warn("out of buffers");
        return;
    }

    buf = b->buffer;
    float* samples = static_cast<float*>(buf->datas[0].data);
    uint32_t n_samples = buf->datas[0].chunk->size / sizeof(float);

    if (samples && n_samples > 0 && impl->m_data_callback) {
        std::vector<float> audio_data(samples, samples + n_samples);
        impl->m_data_callback(audio_data);
    }

    pw_stream_queue_buffer(impl->p_stream, b);
}

audio_manager_impl::audio_manager_impl()
{
}

audio_manager_impl::~audio_manager_impl()
{
    if (p_stream) {
        spdlog::info("Stream destroying. {}", pw_stream_get_name(p_stream));
        pw_stream_destroy(p_stream);
        spdlog::info("Stream destroyed.");
    }

    if (p_main_loop) {
        pw_main_loop_destroy(p_main_loop);
    }

    pw_deinit();
    spdlog::info("Audio manager destroyed.");
}

bool audio_manager_impl::init()
{
    pw_init(nullptr, nullptr);
    log_pipewire_debug_info();

    p_main_loop = pw_main_loop_new(nullptr);

    if (!p_main_loop) {
        spdlog::error("Failed to create main loop");
        return false;
    }
    spdlog::info("Audio manager initialized.");
    return true;
}

bool audio_manager_impl::setup_stream()
{
    auto props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        nullptr);

    const std::string aqua_stream_name = (aqua_core_BINARY_NAME "-capture");

    // 这里使用this作为data,
    p_stream = pw_stream_new_simple(
        pw_main_loop_get_loop(p_main_loop),
        aqua_stream_name.c_str(),
        props,
        &stream_events,
        this);

    if (!p_stream) {
        spdlog::error("Failed to create audio manager stream");
        return false;
    }

    m_builder = SPA_POD_BUILDER_INIT(m_buffer, sizeof(m_buffer));

    spa_audio_info_raw info {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000; // 可以根据需要调整采样率
    info.channels = 2; // 可以根据需要调整通道数

    p_params[0] = spa_format_audio_raw_build(&m_builder, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(p_stream,
            PW_DIRECTION_INPUT,
            PW_ID_ANY,
            // TODO: need to get default audio endpoint.
            static_cast<enum pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            p_params, 1)
        < 0) {
        spdlog::error("Failed to connect stream");
        return false;
    }

    spdlog::info("Stream connected: {}", pw_stream_get_name(p_stream));
    return true;
}

bool audio_manager_impl::start_capture(AudioDataCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_data_callback = std::move(callback);

    if (p_main_loop) {
        pw_main_loop_run(p_main_loop);
        return true;
    }
    return false;
}

bool audio_manager_impl::stop_capture()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (p_main_loop) {
        pw_main_loop_quit(p_main_loop);
        return true;
    }
    return false;
}

#endif // __linux__
