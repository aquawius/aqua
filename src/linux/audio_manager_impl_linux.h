//
// Created by aquawius on 25-1-11.
//

// audio_manager_impl_linux.h
#ifndef AUDIO_MANAGER_IMPL_H
#define AUDIO_MANAGER_IMPL_H
#include <mutex>
#include <functional>
#include <vector>

#ifdef __linux__

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <string>

class audio_manager_impl {
public:
    using AudioDataCallback = std::function<void(const std::vector<float>& audioData)>;

    audio_manager_impl();
    ~audio_manager_impl();

    bool init();
    bool setup_stream();
    bool start_capture(AudioDataCallback callback);
    bool stop_capture();

    struct stream_format {
        uint32_t channels{2};      // 默认立体声
        uint32_t sample_rate{48000}; // 默认采样率
        float peak_volume{0.0f};   // 音量峰值
    };

protected:
    // PipeWire handles
    struct pw_core* p_core{nullptr};
    struct pw_main_loop* p_main_loop{nullptr};
    struct pw_loop* p_loop{nullptr};
    struct pw_context* p_context{nullptr};
    struct pw_stream* p_stream{nullptr};

    // Stream parameters
    const struct spa_pod* p_params[1];
    uint8_t m_buffer[1024];
    struct spa_pod_builder m_builder;
    struct stream_format m_format;

    // Callback
    AudioDataCallback m_data_callback;

    mutable std::mutex m_mutex;

    // Stream callbacks
    static void on_process(void* userdata);

    friend void on_quit(void* userdata, int signal_number);
    friend void on_stream_process_cb(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata);
};

#endif // __linux__
#endif // AUDIO_MANAGER_IMPL_H
