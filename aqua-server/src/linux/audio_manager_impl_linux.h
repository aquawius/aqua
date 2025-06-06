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

#include "audio_manager.h"

#ifdef __linux__

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class audio_manager_impl_linux : public audio_manager
{
public:
    audio_manager_impl_linux();
    ~audio_manager_impl_linux() override;

    bool init() override; // 初始化PipeWire
    bool setup_stream(const AudioFormat format) override; // 配置音频流
    bool start_capture(const AudioDataCallback& callback) override; // 启动捕获
    bool stop_capture() override; // 停止捕获
    [[nodiscard]] bool is_capturing() const override; // 检查捕获状态

    // 用新的格式重启流
    bool reconfigure_stream(const AudioFormat& new_format) override;
    // 获取音频格式
    [[nodiscard]] AudioFormat get_current_format() const override; // 获取首选格式
    [[nodiscard]] AudioFormat get_preferred_format() const override;

    void set_data_callback(AudioDataCallback callback) override;
    void set_peak_callback(AudioPeakCallback callback) override;

private:
    // PipeWire格式转换辅助函数
    static AudioEncoding get_AudioEncoding_from_spa_format(spa_audio_format format);
    static spa_audio_format convert_AudioEncoding_to_spa_audio_format(AudioEncoding encoding);

    struct pw_main_loop* p_main_loop { nullptr };
    struct pw_context* p_context { nullptr };
    struct pw_stream* p_stream { nullptr };

    // 流参数
    const struct spa_pod* p_params[1] { };
    uint8_t m_buffer[1024] { };
    struct spa_pod_builder m_builder { };
    uint32_t m_pw_stream_latency { 512 }; // PipeWire 特有的延迟字段

    // 回调与同步
    AudioDataCallback m_data_callback;
    AudioPeakCallback m_peak_callback;

    mutable std::mutex m_mutex; // for start/stop use
    std::atomic<bool> m_is_capturing { false };
    std::jthread m_capture_thread;
    std::promise<void> m_promise_initialized;

    // 捕获音频处理
    void process_audio_buffer(std::span<const std::byte> audio_buffer) const;
    float get_volume_peak(std::span<const std::byte> audio_buffer, const AudioFormat& format) const;

    // PipeWire回调函数
    friend void on_stream_process_cb(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata, pw_stream_state, pw_stream_state, const char*);
};

#endif // __linux__
#endif // AUDIO_MANAGER_IMPL_H
