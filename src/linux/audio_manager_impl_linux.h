//
// Created by aquawius on 25-1-11.
//

// audio_manager_impl_linux.h

#ifndef AUDIO_MANAGER_IMPL_H
#define AUDIO_MANAGER_IMPL_H

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#ifdef __linux__

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class audio_manager_impl {
public:
    using AudioDataCallback = std::function<void(std::span<const float> audio_data)>;

    audio_manager_impl();
    ~audio_manager_impl();

    struct stream_config {
        uint32_t rate { 48000 };
        uint32_t channels { 2 };
        uint32_t latency { 1024 };  // 单位：帧数
    };

    bool init();                     // 初始化PipeWire
    bool setup_stream();             // 配置音频流
    bool start_capture(AudioDataCallback callback); // 启动捕获
    bool stop_capture();             // 停止捕获
    bool is_capturing() const;       // 检查捕获状态
    const stream_config& get_format() const; // 获取流配置

private:
    struct pw_main_loop* p_main_loop { nullptr };
    struct pw_context* p_context { nullptr };
    struct pw_stream* p_stream { nullptr };

    // 流参数
    const struct spa_pod* p_params[1] {};
    uint8_t m_buffer[1024] {};
    struct spa_pod_builder m_builder {};
    stream_config m_stream_config;

    // 回调与同步
    AudioDataCallback m_data_callback;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_is_capturing { false };
    std::jthread m_capture_thread;
    std::promise<void> m_promise_initialized;

    // PipeWire回调函数
    void process_audio_buffer(std::span<const float> audio_buffer) const;
    friend void on_process(void* userdata);
    friend void on_stream_process_cb(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata, pw_stream_state, pw_stream_state, const char*);
};

#endif // __linux__
#endif // AUDIO_MANAGER_IMPL_H