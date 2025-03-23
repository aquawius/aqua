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
#include <cstddef>

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
    bool setup_stream(AudioFormat format) override; // 配置音频流
    bool start_playback() override; // 开始播放
    bool stop_playback() override; // 停止播放
    bool is_playing() const override; // 检查播放状态
    
    [[nodiscard]] AudioFormat get_current_format() const override; // 获取当前配置
    bool reconfigure_stream(const AudioFormat& new_format) override; // 重新配置流

    bool push_packet_data(std::vector<std::byte>&& packet_data) override;
    void set_peak_callback(AudioPeakCallback callback) override;

private:
    // 播放线程循环函数
    void playback_thread_loop(std::stop_token stop_token);
    
    // 音频处理函数
    void process_audio_buffer(std::span<const std::byte> audio_buffer);
    float get_volume_peak(std::span<const std::byte> audio_buffer, const AudioFormat& format) const;

    // WASAPI格式转换辅助函数
    static AudioEncoding get_AudioEncoding_from_WAVEFORMAT(WAVEFORMATEX* wfx);
    static bool convert_AudioFormat_to_WAVEFORMAT(const AudioFormat& format, WAVEFORMATEX** pp_wave_format);

    adaptive_buffer m_adaptive_buffer; // 自适应缓冲区

private:
    // Windows Core Audio COM接口
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> p_enumerator; // 设备枚举器
    Microsoft::WRL::ComPtr<IMMDevice> p_device; // 音频设备接口
    Microsoft::WRL::ComPtr<IAudioClient> p_audio_client; // 音频客户端
    Microsoft::WRL::ComPtr<IAudioRenderClient> p_render_client; // 播放客户端
    
    WAVEFORMATEX* p_wave_format { nullptr }; // 音频格式
    HANDLE m_hRenderEvent = nullptr;   // 回调模式事件

    std::atomic<bool> m_is_playing { false }; // 播放状态原子标记
    std::jthread m_playback_thread; // 播放线程
    std::promise<void> m_promise_initialized; // 线程初始化同步
    mutable std::mutex m_mutex; // start/stop使用的

    AudioPeakCallback m_peak_callback; // 音频显示用户回调函数

private:

    // 下面的和WASAPI的 流路由 相关, 在切换设备的时候会使用到
    // https://learn.microsoft.com/zh-cn/windows/win32/coreaudio/stream-routing    // 实现音频设备通知回调
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
    std::condition_variable m_device_change_cv;
    std::mutex m_device_change_mutex;
    std::thread m_device_change_thread;
    std::atomic<bool> m_device_changed_thread_exit_flag { false };

    // 启动/停止设备更改处理线程
    void start_device_change_listener();
    void stop_device_change_listener();
};

#endif // defined(_WIN32) || defined(_WIN64)

#endif //AUDIO_PLAYBACK_WINDOWS_H
