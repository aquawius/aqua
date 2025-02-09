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
#include <wrl/implements.h>

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
    bool start_capture(const AudioDataCallback& callback); // 开始捕获
    bool stop_capture(); // 停止捕获
    bool is_capturing() const; // 检查捕获状态
    const stream_config& get_format() const; // 获取当前配置

private:
    // 实现音频设备通知回调
    class DeviceNotifier : public IMMNotificationClient {
    public:
        DeviceNotifier(audio_manager_impl* parent);

        // IUnknown 接口方法
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;

        // IMMNotificationClient 接口方法
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

    private:
        std::atomic<ULONG> m_refCount { 1 };
        audio_manager_impl* m_parent;
    };

    // 注册和注销设备通知
    bool register_device_notifications();
    void unregister_device_notifications();

    // 处理设备更改
    void handle_device_change();

private:
    // Windows Core Audio COM接口
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> p_enumerator; // 设备枚举器
    Microsoft::WRL::ComPtr<IMMDevice> p_device; // 音频设备接口
    Microsoft::WRL::ComPtr<IAudioClient> p_audio_client; // 音频客户端
    Microsoft::WRL::ComPtr<IAudioCaptureClient> p_capture_client; // 捕获客户端

    WAVEFORMATEX* p_wave_format { nullptr }; // 音频格式描述符

    stream_config m_stream_config; // 当前音频流配置
    std::atomic<bool> m_is_capturing { false }; // 捕获状态原子标记
    std::jthread m_capture_thread; // 捕获线程
    std::promise<void> m_promise_initialized; // 线程初始化同步
    mutable std::mutex m_mutex;
    AudioDataCallback m_data_callback; // 当前使用的回调函数
    AudioDataCallback m_user_callback; // 保存的用户回调函数

    // 设备通知相关
    DeviceNotifier* m_device_notifier { nullptr };

    // 音频格式转换工具函数
    void convert_pcm16_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void convert_pcm24_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void convert_pcm32_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);
    void handle_format_conversion(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples);

    // 捕获线程主循环
    void capture_thread_loop(std::stop_token stop_token);

    void process_audio_buffer(std::span<const float> audio_buffer) const;
    inline void display_volume(std::span<const float> data) const;

    // 在 IMMNotificationClient 的回调函数中直接调用了可能导致阻塞的 COM 函数，导致了死锁或卡死

    // 原因：IMMNotificationClient回调函数如OnDefaultDeviceChanged直接调用了handle_device_change()，
    // 而该函数又执行了阻塞式的 COM 操作并获取了互斥锁。根据微软文档，在这些回调函数中进行同步 COM 调用或阻塞操作可能会导致死锁
    // 回调函数不再直接调用handle_device_change()，而是设置一个标志m_device_changed并通知一个条件变量m_device_change_cv
    // 一个单独的线程（m_device_change_thread）等待这个条件变量，并异步处理设备更改

    // 添加一个原子标记，表示设备已更改
    std::atomic<bool> m_device_changed { false };

    // 添加一个条件变量和互斥锁，用于通知设备更改
    std::condition_variable m_device_change_cv;
    std::mutex m_device_change_mutex;

    // 添加一个专门处理设备更改的线程, 处理COM的死锁问题, 需要单独开一个线程.
    std::thread m_device_change_thread;
    // 线程退出标志
    std::atomic<bool> m_device_changed_thread_exit_flag { false };

    // 启动设备更改处理线程的函数
    void start_device_change_listener();

    // 停止设备更改处理线程的函数
    void stop_device_change_listener();
};

#endif // defined(_WIN32) || defined(_WIN64)
#endif // AUDIO_MANAGER_IMPL_H