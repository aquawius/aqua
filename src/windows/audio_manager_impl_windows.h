//
// Created by aquawius on 25-1-11.
//

#ifndef AUDIO_MANAGER_IMPL_H
#define AUDIO_MANAGER_IMPL_H

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

class audio_manager_impl {
public:
    // 音频数据回调类型（数据所有权通过移动语义传递）
    using AudioDataCallback = std::function<void(std::span<const float> audio_data)>;

    audio_manager_impl();
    ~audio_manager_impl();

    // 音频流配置信息
    struct stream_config {
        uint32_t rate { 48000 }; // 采样率
        uint32_t channels { 2 }; // 通道数
        uint32_t latency { 1024 }; // 延迟（帧数，暂未使用）
    };

    // 核心接口
    bool init(); // 初始化COM和音频设备
    bool setup_stream(); // 配置音频流
    bool start_capture(AudioDataCallback callback); // 开始捕获
    bool stop_capture(); // 停止捕获
    bool is_capturing() const; // 检查捕获状态
    const stream_config& get_format() const; // 获取当前配置

private:
    // Windows Core Audio COM接口
    Microsoft::WRL::ComPtr<IMMDevice> p_device; // 音频设备接口
    Microsoft::WRL::ComPtr<IAudioClient> p_audio_client; // 音频客户端
    Microsoft::WRL::ComPtr<IAudioCaptureClient> p_capture_client; // 捕获客户端

    WAVEFORMATEX* p_wave_format { nullptr }; // 音频格式描述符
    HANDLE h_capture_event { nullptr }; // 捕获事件句柄

    stream_config m_stream_config; // 当前音频流配置
    std::atomic<bool> m_is_capturing { false }; // 捕获状态原子标记
    std::jthread m_capture_thread; // 捕获线程
    std::promise<void> m_promise_initialized; // 线程初始化同步
    mutable std::mutex m_mutex; // 操作互斥锁
    AudioDataCallback m_data_callback; // 用户回调函数

    // 音频格式转换工具函数
    void convert_pcm16_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void convert_pcm24_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void convert_pcm32_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void handle_format_conversion(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);

    // 捕获线程主循环
    void capture_thread_loop(std::stop_token stop_token);

    void process_audio_buffer(std::span<const float> audio_buffer) const;
    inline void display_volume(std::span<const float> data) const;
};

#endif // defined(_WIN32) || defined(_WIN64)
#endif // AUDIO_MANAGER_IMPL_H