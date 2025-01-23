//
// Created by aquawius on 25-1-11.
//

// audio_manager_impl_linux.h
#ifndef AUDIO_MANAGER_IMPL_H
#define AUDIO_MANAGER_IMPL_H

#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <future>
#include <thread>
#include <vector>

#ifdef __linux__

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class audio_manager_impl {
public:
    using AudioDataCallback = std::function<void(const std::vector<float>& audioData)>;

    audio_manager_impl();
    ~audio_manager_impl();

    struct stream_config {
        uint32_t rate { 48000 };
        uint32_t channels { 2 };
        uint32_t latency { 1024 };

        // TODO: useless peak_volume
        float peak_volume { 0.0f };
    };

protected:
    /* PipeWire handles
     Because use pw_main_loop_new(); core may not use,
     you can get core use Pipewire core API. */
    // struct pw_core* p_core { nullptr };

    struct pw_main_loop* p_main_loop { nullptr };
    struct pw_loop* p_loop { nullptr };
    struct pw_context* p_context { nullptr };
    struct pw_stream* p_stream { nullptr };

    // Stream parameters
    const struct spa_pod* p_params[1] {};
    uint8_t m_buffer[1024] {};
    struct spa_pod_builder m_builder {};
    struct stream_config m_stream_config;

    // Callback
    AudioDataCallback m_data_callback;

    mutable std::mutex m_mutex;
    std::atomic<bool> m_is_capturing { false };

private:
    std::jthread m_capture_thread;

    // if start_captrue() and stop_capture() invoked immediately,
    // may callback will not recive stop_token.
    // add a promise waiting start_capture() initialized.
    std::promise<void> m_promise_initialized;

    // ///////////////
public:
    bool init();
    bool setup_stream();
    bool start_capture(AudioDataCallback callback);
    bool stop_capture();

    bool is_capturing() const;
    const stream_config& get_format() const;

protected:
    // Stream callbacks
    friend void on_process(void* userdata);
    friend void on_quit(void* userdata, int signal_number);
    friend void on_stream_process_cb(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata);

private:
    void process_audio_buffer(const float* samples, uint32_t n_samples);
};

#endif // __linux__
#endif // AUDIO_MANAGER_IMPL_H
