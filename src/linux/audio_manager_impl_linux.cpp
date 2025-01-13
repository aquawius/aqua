//
// Created by aquawius on 25-1-11.
//

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
    if (!impl || !impl->p_stream) {
        spdlog::error("Invalid stream state");
        return;
    }

    if ((b = pw_stream_dequeue_buffer(impl->p_stream)) == nullptr) {
        spdlog::warn("out of buffers");
        return;
    }

    buf = b->buffer;
    if (!buf || !buf->datas[0].data) {
        spdlog::warn("Invalid buffer data");
        return;
    }

    const float* samples = static_cast<float*>(buf->datas[0].data);
    const uint32_t n_samples = buf->datas[0].chunk->size / sizeof(float);
    const uint32_t n_channels = 2;
    // TODO: n_channels

    if (n_samples == 0 || !samples) {
        return;
    }

    // 计算每个通道的样本数
    const uint32_t samples_per_channel = n_samples / n_channels;

    try {
        // 创建音频数据向量
        std::vector<std::vector<float>> channel_data(n_channels);
        for (uint32_t c = 0; c < n_channels; ++c) {
            channel_data[c].reserve(samples_per_channel);

            // 计算该通道的峰值
            float max_amplitude = 0.0f;
            for (uint32_t i = 0; i < samples_per_channel; ++i) {
                const float sample = samples[i * n_channels + c];
                channel_data[c].push_back(sample);
                max_amplitude = std::max(max_amplitude, std::abs(sample));
            }

            // 更新通道峰值
            impl->m_format.peak_volume = std::max(impl->m_format.peak_volume, max_amplitude);

            // 输出调试信息（可选）
            if (max_amplitude > 0.01f) { // 仅在有明显音量时输出
                const int peak_meter_width = 50;
                int peak_level = static_cast<int>(max_amplitude * peak_meter_width);
                peak_level = std::clamp(peak_level, 0, peak_meter_width);

                std::string meter(peak_level, '#');
                meter.resize(peak_meter_width, '-');

                spdlog::debug("Channel {}: [{}] {:.2f}",
                    c, meter, max_amplitude);
            }
        }

        // 如果有回调，处理音频数据
        if (impl->m_data_callback) {
            // 可以选择传递完整的channel_data或者合并后的数据
            std::vector<float> interleaved_data;
            interleaved_data.reserve(n_samples);

            for (uint32_t i = 0; i < samples_per_channel; ++i) {
                for (uint32_t c = 0; c < n_channels; ++c) {
                    interleaved_data.push_back(channel_data[c][i]);
                }
            }

            impl->m_data_callback(interleaved_data);
        }

    } catch (const std::exception& e) {
        spdlog::error("Error processing audio data: {}", e.what());
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
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        // 坑，关键配置，启用输出设备监听 告诉 PipeWire 这个流要监听（capture）音频输出设备（sink） 本质上是创建了一个
        // "monitor" 流，用于监听系统的音频输出
        PW_KEY_NODE_NAME, aqua_core_BINARY_NAME " capture",
        PW_KEY_NODE_DESCRIPTION, aqua_core_BINARY_NAME " Audio Capture", // 添加描述
        PW_KEY_NODE_LATENCY, "1024/48000", // 设置延迟
        PW_KEY_NODE_RATE, "1/48000", // 设置采样率
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
