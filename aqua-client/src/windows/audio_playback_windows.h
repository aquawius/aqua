//
// Created by QU on 25-2-10.
//

#ifndef AUDIO_PLAYBACK_WINDOWS_H
#define AUDIO_PLAYBACK_WINDOWS_H

#include "adaptive_buffer.h"
#include "audio_playback.h"

#include <atomic>
#include <future>
#include <mutex>
#include <span>
#include <thread>

#if defined(_WIN32) || defined(_WIN64)

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <spdlog/spdlog.h>
#include <wrl/client.h>
#include <wrl/implements.h>

class audio_playback_windows : public audio_playback
{
public:
    audio_playback_windows();
    ~audio_playback_windows() override;

    bool init() override; // 初始化COM和音频设备
    bool setup_stream() override; // 配置音频流
    bool start_playback() override; // 开始播放, 通过回调传入数据
    bool stop_playback() override; // 停止播放
    bool is_playing() const override; // 检查播放状态
    const stream_config& get_format() const override; // 获取当前配置

    bool push_packet_data(const std::vector<uint8_t>& origin_packet_data) override;
    void set_peak_callback(AudioPeakCallback callback) override;

private:
    // 播放线程循环函数
    void playback_thread_loop(std::stop_token stop_token);
    void process_volume_peak(std::span<const float> data) const;

    adaptive_buffer m_adaptive_buffer; // 自适应缓冲区

private:
    // TODO: custom audio format, next version.
    static constexpr WAVEFORMATEX m_wave_format = {
        .wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
        .nChannels = 2,
        .nSamplesPerSec = 48000,
        .nAvgBytesPerSec = 48000 * 2 * sizeof(float),
        .nBlockAlign = 2 * sizeof(float),
        .wBitsPerSample = 32,
        .cbSize = 0
    };

    // 最开始获得音频设备的时候会使用COM获取
    // Windows Core Audio COM接口
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> p_enumerator; // 设备枚举器
    Microsoft::WRL::ComPtr<IMMDevice> p_device; // 音频设备接口
    // 可以使用更新的IAudioClient3(Win10), 但是创建共享流的方式有变化
    Microsoft::WRL::ComPtr<IAudioClient> p_audio_client; // 音频客户端
    Microsoft::WRL::ComPtr<IAudioRenderClient> p_render_client; // 播放客户端

    HANDLE h_event { nullptr };
    UINT32 m_buffer_frame_count { 0 };

    stream_config m_stream_config; // 当前音频流配置
    std::atomic<bool> m_is_playing { false }; // 播放状态原子标记
    std::jthread m_playback_thread; // 播放线程
    std::promise<void> m_promise_initialized; // 线程初始化同步
    mutable std::mutex m_mutex; // start stop使用的

    AudioPeakCallback m_peak_callback; // 音频显示用户回调函数
private:
    // 下面的和WASAPI的 流路由 相关, 在切换设备的时候会使用到
    // https://learn.microsoft.com/zh-cn/windows/win32/coreaudio/stream-routing

    // 实现音频设备通知回调
    class DeviceNotifier final : public IMMNotificationClient
    {
    public:
        virtual ~DeviceNotifier() = default;
        explicit DeviceNotifier(audio_playback_windows* parent);

        // IUnknown 接口方法
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;

        // IMMNotificationClient 接口方法
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
        HRESULT STDMETHODCALLTYPE
        OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

    private:
        std::atomic<ULONG> m_ref_count { 1 };
        audio_playback_windows* m_parent;
    };

    // 设备通知类的实例
    DeviceNotifier* m_device_notifier { nullptr };

    // 注册和注销设备通知
    bool register_device_notifications();
    void unregister_device_notifications();

    // 处理设备更改回调函数
    void handle_device_change();

    // 在 IMMNotificationClient 的回调函数中直接调用了可能导致阻塞的 COM 函数，导致了死锁或卡死

    // 原因：IMMNotificationClient回调函数如OnDefaultDeviceChanged直接调用了handle_device_change()，
    // 而该函数又执行了阻塞式的 COM 操作并获取了互斥锁。根据微软文档，在这些回调函数中进行同步 COM 调用或阻塞操作可能会导致死锁
    // 回调函数不再直接调用handle_device_change()(不能直接调)，而是设置一个标志m_device_changed并通知一个条件变量m_device_change_cv
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

#endif //AUDIO_PLAYBACK_WINDOWS_H
