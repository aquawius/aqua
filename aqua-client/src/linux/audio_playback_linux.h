//
// Created by aquawius on 25-1-31.
//

#ifndef AUDIO_PLAYBACK_LINUX_H
#define AUDIO_PLAYBACK_LINUX_H

#include "adaptive_buffer.h"
#include "audio_playback.h"

#ifdef __linux__

#include <atomic>
#include <future>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <vector>
#include <cstddef>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class audio_playback_linux : public audio_playback
{
public:
    audio_playback_linux();
    ~audio_playback_linux() override;

    bool init() override; // 初始化 PipeWire
    bool setup_stream() override; // 配置音频流
    bool start_playback() override; // 启动播放
    bool stop_playback() override; // 停止播放
    bool is_playing() const override; // 检查播放状态

    [[nodiscard]] AudioFormat get_current_format() const override;
    void set_format(AudioFormat) override;

    // 更新为使用字节跨度代替特定格式向量
    bool push_packet_data(std::span<const std::byte> packet_data) override;
    void set_peak_callback(AudioPeakCallback callback) override;

private:
    struct pw_main_loop* p_main_loop { nullptr };
    struct pw_context* p_context { nullptr };
    struct pw_stream* p_stream { nullptr };

    // 流参数
    const struct spa_pod* p_params[1] { };
    uint8_t m_buffer[1024] { };
    struct spa_pod_builder m_builder { };
    uint32_t m_pw_stream_latency { 1024 }; // PipeWire 特有的延迟字段

    // 同步控制
    mutable std::mutex m_mutex;
    std::atomic<bool> m_is_playing { false };
    std::jthread m_playback_thread;
    std::promise<void> m_promise_initialized;

    AudioPeakCallback m_peak_callback; // 音频显示用户回调函数

    // PipeWire 回调函数
    void process_playback_buffer();
    void process_volume_peak(std::span<const float> data) const;

    friend void on_playback_process(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata, pw_stream_state old, pw_stream_state state,
                                           const char* error);

    // 音频包头结构
    struct AudioPacketHeader
    {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp; // 时间戳
    } __attribute__((packed));

    adaptive_buffer m_adaptive_buffer; // 替换原来的 m_packets_deque
};

#endif // __linux__
#endif // AUDIO_PLAYBACK_LINUX_H
