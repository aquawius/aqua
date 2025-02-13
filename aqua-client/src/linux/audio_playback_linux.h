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

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

class audio_playback_linux : public audio_playback {
public:
    audio_playback_linux();
    ~audio_playback_linux() override;

    bool init() override; // 初始化 PipeWire
    bool setup_stream() override; // 配置音频流
    bool start_playback() override; // 启动播放
    bool stop_playback() override; // 停止播放
    bool is_playing() const override; // 检查播放状态
    const stream_config& get_format() const override; // 获取流配置

    // 写入音频数据，以供播放（由网络层调用）
    bool push_packet_data(const std::vector<uint8_t>& origin_packet_data) override;

private:
    struct pw_main_loop* p_main_loop { nullptr };
    struct pw_context* p_context { nullptr };
    struct pw_stream* p_stream { nullptr };

    // 流参数
    const struct spa_pod* p_params[1] {};
    uint8_t m_buffer[1024] {};
    struct spa_pod_builder m_builder {};
    stream_config m_stream_config;

    // 同步控制
    mutable std::mutex m_mutex;
    std::atomic<bool> m_is_playing { false };
    std::jthread m_playback_thread;
    std::promise<void> m_promise_initialized;

    // PipeWire 回调函数
    void process_playback_buffer();
    void display_volume(std::span<const float> data) const;

    friend void on_playback_process(void* userdata);
    friend void on_stream_state_changed_cb(void* userdata, pw_stream_state old, pw_stream_state state, const char* error);

    // 音频包头结构
    struct AudioPacketHeader {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp; // 时间戳
    } __attribute__((packed));

    adaptive_buffer m_adaptive_buffer; // 替换原来的 m_packets_deque
};

#endif // __linux__
#endif // AUDIO_PLAYBACK_LINUX_H