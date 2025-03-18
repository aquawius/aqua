//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_windows.h"

#include <functiondiscoverykeys_devpkey.h>

#include <array>
#include <chrono>

#if defined(_WIN32) || defined(_WIN64)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")

// 构造函数：初始化成员变量
audio_manager_impl_windows::audio_manager_impl_windows()
{
    start_device_change_listener();
    spdlog::debug("[audio_manager] Audio manager instance created.");
}

// 析构函数：资源清理
audio_manager_impl_windows::~audio_manager_impl_windows()
{
    stop_device_change_listener();

    stop_capture();
    unregister_device_notifications();

    // 释放音频格式内存
    if (p_wave_format) {
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
        spdlog::debug("[audio_manager] Wave format released.");
    }

    if (m_hCaptureEvent) {
        CloseHandle(m_hCaptureEvent);
        m_hCaptureEvent = nullptr;
    }

    // 释放COM接口
    p_capture_client.Reset();
    p_audio_client.Reset();
    p_device.Reset();
    p_enumerator.Reset();

    // 反初始化COM库
    CoUninitialize();
    spdlog::info("[audio_manager] Audio manager destroyed.");
}

// 初始化COM库并获取默认音频设备
bool audio_manager_impl_windows::init()
{
    // 初始化设备更改线程的COM为STA
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] COM initialization failed: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("[audio_manager] COM library initialized.");

    // 创建设备枚举器
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&p_enumerator));
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to create device enumerator: HRESULT {0:x}", hr);
        return false;
    }

    // 注册设备通知
    if (!register_device_notifications()) {
        spdlog::error("[audio_manager] Failed to register device notifications.");
        return false;
    }

    // 获取默认音频输出设备
    hr = p_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_device);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to get default audio endpoint: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("[audio_manager] Default audio endpoint acquired.");
    return true;
}

// 配置音频流参数并初始化客户端
bool audio_manager_impl_windows::setup_stream()
{
    HRESULT hr = S_OK;
    spdlog::debug("[audio_manager] Entering setup_stream().");

    // 释放之前的资源（如果有）
    if (p_audio_client) {
        hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("[audio_manager] Failed to stop previous audio client: HRESULT {0:x}", hr);
        }
    }
    p_capture_client.Reset();
    p_audio_client.Reset();

    spdlog::debug("[audio_manager] Activating audio client.");
    hr = p_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &p_audio_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to activate audio client: HRESULT {0:x}", hr);
        return false;
    }

    // 获取音频混合格式
    if (p_wave_format) {
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
    }

    spdlog::debug("[audio_manager] Getting mix format.");
    hr = p_audio_client->GetMixFormat(&p_wave_format);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to get mix format: HRESULT {0:x}", hr);
        return false;
    }
    if (p_wave_format == nullptr) {
        spdlog::error("[audio_manager] Mix format is null.");
        return false;
    }

    m_stream_config.encoding = wave_format_to_encoding(p_wave_format);
    m_stream_config.channels = p_wave_format->nChannels;
    m_stream_config.bit_depth = p_wave_format->wBitsPerSample;
    m_stream_config.sample_rate = p_wave_format->nSamplesPerSec;

    spdlog::info("[audio_manager] Audio format: {} Hz, {} channels, {} bits/sample",
        p_wave_format->nSamplesPerSec,
        p_wave_format->nChannels,
        p_wave_format->wBitsPerSample);

    // 初始化音频客户端
    constexpr REFERENCE_TIME buffer_duration = 20 * 1000; // 20ms 音频系统内部缓冲区的容量
    spdlog::debug("[audio_manager] Initializing audio client.");
    hr = p_audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buffer_duration,
        0,
        p_wave_format,
        nullptr);

    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Audio client initialization failed: HRESULT {0:x}", hr);
        return false;
    }

    // 创建事件对象
    m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_hCaptureEvent == nullptr) {
        spdlog::error("[audio_manager] CreateEvent failed: {}", GetLastError());
        return false;
    }

    // 设置事件句柄
    hr = p_audio_client->SetEventHandle(m_hCaptureEvent);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] SetEventHandle failed: HRESULT {0:x}", hr);
        CloseHandle(m_hCaptureEvent);
        return false;
    }

    // 获取捕获客户端接口
    spdlog::debug("[audio_manager] Getting capture client.");
    hr = p_audio_client->GetService(__uuidof(IAudioCaptureClient), &p_capture_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to get capture client: HRESULT {0:x}", hr);
        return false;
    }



    spdlog::debug("[audio_manager] Exiting setup_stream().");
    return true;
}

// 启动音频捕获线程
bool audio_manager_impl_windows::start_capture(const AudioDataCallback& callback)
{
    std::lock_guard lock(m_mutex);
    spdlog::debug("[audio_manager] Attempting to start capture.");

    if (m_is_capturing) {
        spdlog::warn("[audio_manager] Capture already running. Ignoring start request.");
        return false;
    }

    set_data_callback(callback); // 复制回调函数
    m_user_callback = callback; // 保存用户回调函数
    m_promise_initialized = std::promise<void>();

    // 启动音频客户端
    spdlog::info("[audio_manager] Starting audio client and capture thread.");

    HRESULT hr = p_audio_client->Start();
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to start audio client: HRESULT {0:x}", hr);
        return false;
    }

    spdlog::info("[audio_manager] Audio client started.");

    // 启动捕获线程
    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_capturing = true;
        m_promise_initialized.set_value();
        spdlog::info("[audio_manager] Capture thread started.");
        capture_thread_loop(stop_token);
        m_is_capturing = false;
        spdlog::info("[audio_manager] Capture thread stopped.");
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

// 停止捕获并清理资源
bool audio_manager_impl_windows::stop_capture()
{
    std::lock_guard lock(m_mutex);
    if (!m_is_capturing) {
        spdlog::warn("[audio_manager] No active capture to stop.");
        return false;
    }

    // 停止音频流
    if (p_audio_client) {
        HRESULT hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("[audio_manager] Failed to stop audio client: HRESULT {0:x}", hr);
        }
    }

    // 请求线程停止并等待退出
    m_capture_thread.request_stop();
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
        spdlog::debug("[audio_manager] Capture thread joined.");
    }

    m_is_capturing = false;
    return true;
}

// 查询当前捕获状态
bool audio_manager_impl_windows::is_capturing() const
{
    return m_is_capturing.load();
}

void audio_manager_impl_windows::set_data_callback(AudioDataCallback callback)
{
    m_data_callback = std::move(callback);
}

void audio_manager_impl_windows::set_peak_callback(AudioPeakCallback callback)
{
    m_peak_callback = std::move(callback);
}

// 捕获线程主循环
void audio_manager_impl_windows::capture_thread_loop(std::stop_token stop_token)
{
    const UINT32 channels = p_wave_format->nChannels;
    const UINT32 bytes_per_sample = p_wave_format->wBitsPerSample / 8;

    while (!stop_token.stop_requested()) {
        DWORD waitResult = WaitForSingleObject(m_hCaptureEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            UINT32 packetLength = 0;
            HRESULT hr = p_capture_client->GetNextPacketSize(&packetLength);

            if (FAILED(hr)) {
                spdlog::error("[audio_manager] GetNextPacketSize failed: HRESULT {0:x}", hr);
                break;
            }

            // 处理所有就绪的数据包
            while (packetLength > 0) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;

                // 获取音频缓冲区
                hr = p_capture_client->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    spdlog::error("[audio_manager] GetBuffer failed: HRESULT {0:x}", hr);
                    p_audio_client->Stop(); // 确保停止音频流
                    break;
                }

                // 处理静音或有效数据
                const UINT32 numSamples = numFrames * channels;
                // 根据实际格式计算字节大小
                const size_t rawByteSize = numSamples * bytes_per_sample;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // 静音数据，创建零填充的字节缓冲区
                    std::vector<std::byte> silentBuffer(rawByteSize, static_cast<std::byte>(0));
                    process_audio_buffer(std::span<const std::byte>(silentBuffer));
                } else {
                    // 有效数据，直接传递原始字节
                    process_audio_buffer(std::span<const std::byte>(reinterpret_cast<const std::byte*>(pData), rawByteSize));
                }

                // 释放缓冲区并获取下一个包大小
                hr = p_capture_client->ReleaseBuffer(numFrames);
                if (FAILED(hr)) {
                    spdlog::error("[audio_manager] ReleaseBuffer failed: HRESULT {0:x}", hr);
                    break;
                }

                hr = p_capture_client->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    spdlog::error("[audio_manager] GetNextPacketSize failed: HRESULT {0:x}", hr);
                    break;
                }
            }
        } else if (waitResult == WAIT_FAILED) {
            spdlog::error("[audio_manager] WaitForSingleObject failed: {}", GetLastError());
            break;
        }
    }
}
void audio_manager_impl_windows::process_audio_buffer(std::span<const std::byte> audio_buffer) const
{
    if (audio_buffer.empty()) return;

    // 直接传递原始数据
    if (m_data_callback) {
        m_data_callback(audio_buffer);
    }

    // 峰值计算
    if (!m_peak_callback) return;

    const auto format = get_preferred_format();
    if (format.encoding == AudioEncoding::INVALID) return;

    const float peak_value = get_volume_peak(audio_buffer, format);
    m_peak_callback(peak_value);
}

// 新增的峰值计算函数（带采样优化）
float audio_manager_impl_windows::get_volume_peak(std::span<const std::byte> audio_buffer, const AudioFormat& format) const
{
    constexpr size_t MAX_SAMPLES = 100; // 最多处理1000个采样点
    const size_t sample_size = format.bit_depth / 8;
    const size_t total_samples = audio_buffer.size() / sample_size;
    const size_t step = (std::max<size_t>)(1, total_samples / MAX_SAMPLES);

    float max_peak = 0.0f;
    size_t processed_samples = 0;

    switch (format.encoding) {
    case AudioEncoding::PCM_F32LE: {
        auto* data = reinterpret_cast<const float*>(audio_buffer.data());
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i]));
            if (++processed_samples >= MAX_SAMPLES) break;
        }
        break;
    }
    case AudioEncoding::PCM_S16LE: {
        auto* data = reinterpret_cast<const int16_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 32768.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES) break;
        }
        break;
    }
    case AudioEncoding::PCM_S24LE: {
        const auto* data = reinterpret_cast<const uint8_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 8388608.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            // 24-bit小端处理优化：直接内存访问
            int32_t sample = static_cast<int32_t>(data[0] << 8 | data[1] << 16 | data[2] << 24);
            sample >>= 8; // 符号扩展
            max_peak = (std::max)(max_peak, std::abs(sample * scale));
            data += 3 * step * format.channels;
            if (++processed_samples >= MAX_SAMPLES) break;
        }
        break;
    }
    case AudioEncoding::PCM_S32LE: {
        auto* data = reinterpret_cast<const int32_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES) break;
        }
        break;
    }
    case AudioEncoding::PCM_U8: {
        auto* data = reinterpret_cast<const uint8_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 128.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs((data[i] - 128) * scale));
            if (++processed_samples >= MAX_SAMPLES) break;
        }
        break;
    }
    default:
        return 0.0f;
    }

    return max_peak;
}

// 简化后的格式获取
audio_manager::AudioFormat audio_manager_impl_windows::get_preferred_format() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    AudioFormat format { };

    if (p_wave_format) {
        format.sample_rate = p_wave_format->nSamplesPerSec;
        format.channels = p_wave_format->nChannels;
        format.bit_depth = p_wave_format->wBitsPerSample;
        format.encoding = wave_format_to_encoding(p_wave_format);
    } else {
        format.encoding = AudioEncoding::INVALID;
    }

    return format;
}

audio_manager::AudioEncoding audio_manager_impl_windows::wave_format_to_encoding(WAVEFORMATEX* wfx)
{
    if (!wfx) return AudioEncoding::INVALID;

    // 基本格式判断
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return (wfx->wBitsPerSample == 32) ? AudioEncoding::PCM_F32LE : AudioEncoding::INVALID;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        switch (wfx->wBitsPerSample) {
        case 8:  return AudioEncoding::PCM_U8;
        case 16: return AudioEncoding::PCM_S16LE;
        case 24: return AudioEncoding::PCM_S24LE;
        case 32: return AudioEncoding::PCM_S32LE;
        default: return AudioEncoding::INVALID;
        }
    }

    // 扩展格式判断
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            return (wfx->wBitsPerSample == 32) ? AudioEncoding::PCM_F32LE : AudioEncoding::INVALID;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            switch (wfx->wBitsPerSample) {
            case 8:  return AudioEncoding::PCM_U8;
            case 16: return AudioEncoding::PCM_S16LE;
            case 24: return AudioEncoding::PCM_S24LE;
            case 32: return AudioEncoding::PCM_S32LE;
            default: return AudioEncoding::INVALID;
            }
        }
    }

    return AudioEncoding::INVALID;
}

// 实现DeviceNotifier类
audio_manager_impl_windows::DeviceNotifier::DeviceNotifier(audio_manager_impl_windows* parent)
    : m_parent(parent) {}

ULONG STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::AddRef()
{
    return m_ref_count.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::Release()
{
    ULONG ref = m_ref_count.fetch_sub(1) - 1;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::QueryInterface(REFIID riid, void** ppvObject)
{
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
    spdlog::info("[audio_manager] Device state changed.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_manager] Device added.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_manager] Device removed.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)
{
    if (flow == eRender && role == eConsole) {
        spdlog::info("[audio_manager] Default device changed.");
        m_parent->m_device_changed.store(true);
        m_parent->m_device_change_cv.notify_one();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_manager_impl_windows::DeviceNotifier::OnPropertyValueChanged(
    LPCWSTR pwstrDeviceId, const PROPERTYKEY key)
{
    // 不再检查特定的键值，而是简单记录属性变化但不触发重启
    spdlog::debug("[audio_manager] Device property value changed.");

    // 不做任何操作，忽略属性变化
    return S_OK;
}

bool audio_manager_impl_windows::register_device_notifications()
{
    m_device_notifier = new DeviceNotifier(this);
    HRESULT hr = p_enumerator->RegisterEndpointNotificationCallback(m_device_notifier);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] RegisterEndpointNotificationCallback failed: HRESULT {0:x}", hr);
        delete m_device_notifier;
        m_device_notifier = nullptr;
        return false;
    }
    return true;
}

void audio_manager_impl_windows::unregister_device_notifications()
{
    if (m_device_notifier) {
        p_enumerator->UnregisterEndpointNotificationCallback(m_device_notifier);
        m_device_notifier->Release();
        m_device_notifier = nullptr;
    }
}

void audio_manager_impl_windows::handle_device_change()
{
    spdlog::info("[audio_manager] Handling device change.");

    // 停止当前捕获
    if (m_is_capturing) {
        spdlog::debug("[audio_manager] Stopping current capture.");
        if (stop_capture()) {
            spdlog::info("[audio_manager] Capture stopped.");
        } else {
            spdlog::error("[audio_manager] Capture failed.");
            throw std::runtime_error("[audio_manager] Stop capture failed.");
        }
    }

    // 重新获取默认设备
    spdlog::debug("[audio_manager] Acquiring new default audio endpoint.");
    p_device.Reset();
    HRESULT hr = p_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_device);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] Failed to get default audio endpoint after device change: HRESULT {0:x}", hr);
        return;
    }

    // 重新设置流
    spdlog::debug("[audio_manager] Setting up new audio stream.");
    if (!setup_stream()) {
        spdlog::error("[audio_manager] Failed to setup stream after device change.");
        return;
    }

    // 重新开始捕获，使用保存的用户回调函数
    spdlog::debug("[audio_manager] Restarting capture.");
    if (!start_capture(m_user_callback)) {
        spdlog::error("[audio_manager] Failed to restart capture after device change.");
        // TODO: notify parent level to handle exception.
        return;
    }

    spdlog::info("[audio_manager] Device change handled successfully.");
}

void audio_manager_impl_windows::start_device_change_listener()
{
    m_device_change_thread = std::thread([this]() {
        // 初始化设备更改线程的COM为STA
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            spdlog::error("[audio_manager] Device change thread COM init failed: {0:x}", hr);
            return;
        }

        std::unique_lock<std::mutex> lock(m_device_change_mutex);
        while (true) {
            m_device_change_cv.wait(lock, [this]() { return m_device_changed.load() || m_device_changed_thread_exit_flag.load(); });

            if (m_device_changed_thread_exit_flag.load()) {
                spdlog::debug("[audio_manager] Device change listener thread exiting.");
                break;
            }

            if (m_device_changed.load()) {
                m_device_changed.store(false);

                // 在调用 handle_device_change() 之前解锁，避免死锁
                lock.unlock();
                handle_device_change();
                lock.lock();
            }
        }

        CoUninitialize();
    });
}

void audio_manager_impl_windows::stop_device_change_listener()
{
    // 设置退出标志，并通知线程
    // 程序结束时正确通知设备更改监听线程退出
    {
        std::lock_guard<std::mutex> lock(m_device_change_mutex);
        m_device_changed_thread_exit_flag.store(true);
    }
    m_device_change_cv.notify_one();

    if (m_device_change_thread.joinable()) {
        m_device_change_thread.join();
    }
}

#endif // defined(_WIN32) || defined(_WIN64)