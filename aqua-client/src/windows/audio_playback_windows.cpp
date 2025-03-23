//
// Created by QU on 25-2-10.
//

#include "audio_playback_windows.h"

#include <array>
#include <chrono>
#include <algorithm>

#if defined(_WIN32) || defined(_WIN64)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

audio_playback_windows::audio_playback_windows()
{
    start_device_change_listener();
    spdlog::debug("[audio_playback] Audio playback instance created.");
}

audio_playback_windows::~audio_playback_windows()
{
    stop_device_change_listener();
    stop_playback();
    unregister_device_notifications();

    if (p_wave_format) {
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
    }

    if (m_hRenderEvent) {
        CloseHandle(m_hRenderEvent);
        m_hRenderEvent = nullptr;
    }

    // 释放COM接口
    p_render_client.Reset();
    p_audio_client.Reset();
    p_device.Reset();
    p_enumerator.Reset();

    // 反初始化COM库
    CoUninitialize();
    spdlog::info("[audio_playback] Audio playback destroyed.");
}

// 初始化COM库并获取默认音频设备
bool audio_playback_windows::init()
{
    // 初始化COM为STA
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] COM initialization failed: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("[audio_playback] COM library initialized.");

    // 创建设备枚举器
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&p_enumerator));
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to create device enumerator: HRESULT {0:x}", hr);
        return false;
    }

    // 注册设备通知
    if (!register_device_notifications()) {
        spdlog::error("[audio_playback] Failed to register device notifications.");
        return false;
    }

    // 获取默认音频输出设备
    hr = p_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_device);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to get default audio endpoint: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("[audio_playback] Default audio endpoint acquired.");
    return true;
}

bool audio_playback_windows::setup_stream(AudioFormat format)
{
    HRESULT hr = S_OK;
    spdlog::debug("[audio_playback] Setting up audio stream.");

    if (!AudioFormat::is_valid(format)) {
        spdlog::error("[audio_playback] Invalid audio format provided.");
        return false;
    }

    // 释放之前的资源（如果有）
    if (p_audio_client) {
        hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("[audio_playback] Failed to stop previous audio client: HRESULT {0:x}", hr);
        }
    }
    p_render_client.Reset();
    p_audio_client.Reset();

    // 释放之前的格式（如果有）
    if (p_wave_format) {
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
    }

    spdlog::debug("[audio_playback] Activating audio client.");
    hr = p_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &p_audio_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to activate audio client: HRESULT {0:x}", hr);
        return false;
    }

    // 获取混合格式
    spdlog::debug("[audio_playback] Getting mix format.");
    hr = p_audio_client->GetMixFormat(&p_wave_format);
    if (FAILED(hr) || !p_wave_format) {
        spdlog::error("[audio_playback] Failed to get mix format: HRESULT {0:x}", hr);
        return false;
    }

    // 创建用户请求的格式
    WAVEFORMATEX* p_requested_format = nullptr;
    bool format_creation_succeeded = convert_AudioFormat_to_WAVEFORMAT(format, &p_requested_format);

    if (!format_creation_succeeded || !p_requested_format) {
        spdlog::error("[audio_playback] Failed to create requested format.");
        return false;
    }

    // 检查请求的格式是否被支持
    WAVEFORMATEX* p_closest_format = nullptr;
    hr = p_audio_client->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED,
        p_requested_format,
        &p_closest_format);

    if (hr == S_OK) {
        // 完全支持请求的格式
        spdlog::info("[audio_playback] Requested format is fully supported.");
        CoTaskMemFree(p_wave_format);
        p_wave_format = p_requested_format;
    } else if (hr == S_FALSE && p_closest_format) {
        // 有类似的支持格式，但我们不使用
        spdlog::error("[audio_playback] Format not exactly supported.");
        CoTaskMemFree(p_requested_format);
        CoTaskMemFree(p_closest_format);
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
        return false;
    } else {
        // 不支持
        spdlog::error("[audio_playback] Format not supported.");
        CoTaskMemFree(p_requested_format);
        if (p_closest_format) {
            CoTaskMemFree(p_closest_format);
        }
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
        return false;
    }

    // 更新流配置为实际使用的格式
    m_stream_config.encoding = get_AudioEncoding_from_WAVEFORMAT(p_wave_format);
    m_stream_config.channels = p_wave_format->nChannels;
    m_stream_config.bit_depth = p_wave_format->wBitsPerSample;
    m_stream_config.sample_rate = p_wave_format->nSamplesPerSec;

    spdlog::info("[audio_playback] Using audio format: {} Hz, {} channels, {} bits/sample, encoding:{}",
        m_stream_config.sample_rate,
        m_stream_config.channels,
        m_stream_config.bit_depth,
        static_cast<int>(m_stream_config.encoding));

    // 初始化音频客户端
    constexpr REFERENCE_TIME buffer_duration = 100 * 1000; // 100ms
    spdlog::debug("[audio_playback] Initializing audio client.");
    hr = p_audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buffer_duration,
        0,
        p_wave_format,
        nullptr);

    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Audio client initialization failed: HRESULT {0:x}", hr);
        return false;
    }

    // 创建事件对象
    if (!m_hRenderEvent) {
        m_hRenderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_hRenderEvent) {
            spdlog::error("[audio_playback] CreateEvent failed: {}", GetLastError());
            return false;
        }
    }

    // 设置事件句柄
    hr = p_audio_client->SetEventHandle(m_hRenderEvent);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] SetEventHandle failed: HRESULT {0:x}", hr);
        return false;
    }

    // 获取播放客户端接口
    spdlog::debug("[audio_playback] Getting render client.");
    hr = p_audio_client->GetService(__uuidof(IAudioRenderClient), &p_render_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to get render client: HRESULT {0:x}", hr);
        return false;
    }

    spdlog::debug("[audio_playback] Audio stream setup complete.");
    return true;
}

bool audio_playback_windows::reconfigure_stream(const AudioFormat& new_format)
{
    // 如果格式相同，无需重新配置
    if (new_format == m_stream_config) {
        spdlog::debug("[audio_playback] Format unchanged, skipping reconfiguration");
        return true;
    }

    bool was_playing = false;

    {
        std::lock_guard lock(m_mutex);
        was_playing = m_is_playing;

        spdlog::info("[audio_playback] Reconfiguring stream from {}Hz, {}ch to {}Hz, {}ch",
            m_stream_config.sample_rate, m_stream_config.channels,
            new_format.sample_rate, new_format.channels);
    }

    // 在锁外进行可能阻塞的操作，避免递归锁定

    // 停止当前播放（如果正在进行）
    if (was_playing) {
        if (!stop_playback()) {
            spdlog::error("[audio_playback] Failed to stop playback during reconfiguration.");
            return false;
        }
    }

    // 使用新格式重新配置流
    if (!setup_stream(new_format)) {
        spdlog::error("[audio_playback] Failed to setup stream with new format.");
        return false;
    }

    // 如果之前正在播放，则恢复播放
    if (was_playing) {
        if (!start_playback()) {
            spdlog::error("[audio_playback] Failed to restart playback after reconfiguration.");
            return false;
        }
    }

    spdlog::info("[audio_playback] Stream reconfigured successfully.");
    return true;
}

bool audio_playback_windows::start_playback()
{
    std::lock_guard lock(m_mutex);
    spdlog::debug("[audio_playback] Attempting to start playback.");

    if (m_is_playing) {
        spdlog::warn("[audio_playback] Playback already running. Ignoring start request.");
        return false;
    }

    m_promise_initialized = std::promise<void>();

    // 启动音频客户端
    spdlog::info("[audio_playback] Starting audio client and playback thread.");

    HRESULT hr = p_audio_client->Start();
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to start audio client: HRESULT {0:x}", hr);
        return false;
    }

    spdlog::info("[audio_playback] Audio client started.");

    // 启动播放线程
    m_playback_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_playing = true;
        m_promise_initialized.set_value();
        spdlog::info("[audio_playback] Playback thread started.");
        playback_thread_loop(stop_token);
        m_is_playing = false;
        spdlog::info("[audio_playback] Playback thread stopped.");
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

bool audio_playback_windows::stop_playback()
{
    std::lock_guard lock(m_mutex);
    if (!m_is_playing) {
        spdlog::warn("[audio_playback] No active playback to stop.");
        return false;
    }

    // 停止音频流
    if (p_audio_client) {
        HRESULT hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("[audio_playback] Failed to stop audio client: HRESULT {0:x}", hr);
        }
    }

    // 请求线程停止并等待退出
    m_playback_thread.request_stop();
    if (m_playback_thread.joinable()) {
        m_playback_thread.join();
        spdlog::debug("[audio_playback] Playback thread joined.");
    }

    m_is_playing = false;
    return true;
}

bool audio_playback_windows::is_playing() const
{
    return m_is_playing;
}

audio_playback::AudioFormat audio_playback_windows::get_current_format() const
{
    return m_stream_config;
}

bool audio_playback_windows::push_packet_data(std::vector<std::byte>&& packet_data)
{
    if (packet_data.empty()) {
        spdlog::warn("[audio_playback] Empty packet data received");
        return false;
    }

    return m_adaptive_buffer.push_buffer_packets(std::move(packet_data));
}

void audio_playback_windows::set_peak_callback(AudioPeakCallback callback)
{
    m_peak_callback = std::move(callback);
}

void audio_playback_windows::playback_thread_loop(std::stop_token stop_token)
{
    UINT32 buffer_total_frames = 0;
    HRESULT hr = p_audio_client->GetBufferSize(&buffer_total_frames);
    if (FAILED(hr)) {
        spdlog::critical("[audio_playback] Failed to get audio buffer size: HRESULT {0:x}", hr);
        return;
    }

    // 计算每帧字节数
    const UINT32 bytes_per_frame = p_wave_format->nChannels * (p_wave_format->wBitsPerSample / 8);

    while (!stop_token.stop_requested()) {
        DWORD waitResult = WaitForSingleObject(m_hRenderEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            // 获取当前填充量
            UINT32 padding_frames = 0;
            hr = p_audio_client->GetCurrentPadding(&padding_frames);
            if (FAILED(hr)) {
                spdlog::warn("[audio_playback] GetCurrentPadding failed: HRESULT {0:x}", hr);
                continue;
            }

            const UINT32 available_frames = buffer_total_frames - padding_frames;
            if (available_frames == 0) {
                continue;
            }

            BYTE* p_audio_data = nullptr;
            hr = p_render_client->GetBuffer(available_frames, &p_audio_data);
            if (FAILED(hr)) {
                spdlog::warn("[audio_playback] GetBuffer failed: HRESULT {0:x}", hr);
                continue;
            }

            // 计算需要的字节数
            const size_t needed_bytes = available_frames * bytes_per_frame;

            // 从自适应缓冲区获取字节数据
            std::vector<uint8_t> buffer_data(needed_bytes);
            const size_t filled_bytes = m_adaptive_buffer.pull_buffer_data(
                buffer_data.data(), needed_bytes);

            if (filled_bytes > 0) {
                // 将字节数据复制到音频缓冲区
                std::memcpy(p_audio_data, buffer_data.data(), filled_bytes);

                // 如果需要，填充剩余部分为静音
                if (filled_bytes < needed_bytes) {
                    std::memset(p_audio_data + filled_bytes, 0, needed_bytes - filled_bytes);
                }

                // 处理音频数据以计算音量峰值
                std::span<const std::byte> audio_span(
                    reinterpret_cast<const std::byte*>(buffer_data.data()),
                    filled_bytes);
                process_audio_buffer(audio_span);

                // 释放缓冲区
                hr = p_render_client->ReleaseBuffer(available_frames, 0);
                if (FAILED(hr)) {
                    spdlog::warn("[audio_playback] ReleaseBuffer failed: HRESULT {0:x}", hr);
                }
            } else {
                // 填充静音
                hr = p_render_client->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                if (FAILED(hr)) {
                    spdlog::warn("[audio_playback] ReleaseBuffer (silent) failed: HRESULT {0:x}", hr);
                }
            }
        } else if (waitResult == WAIT_FAILED) {
            spdlog::error("[audio_playback] WaitForSingleObject failed: {}", GetLastError());
            break;
        }
    }
}

void audio_playback_windows::process_audio_buffer(std::span<const std::byte> audio_buffer)
{
    if (audio_buffer.empty() || !m_peak_callback)
        return;

    const auto& format = m_stream_config;
    if (format.encoding == AudioEncoding::INVALID)
        return;

    const float peak_value = get_volume_peak(audio_buffer, format);
    m_peak_callback(peak_value);
}

float audio_playback_windows::get_volume_peak(std::span<const std::byte> audio_buffer, const AudioFormat& format) const
{
    constexpr size_t MAX_SAMPLES = 100; // 最多处理100个采样点
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
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_S16LE: {
        auto* data = reinterpret_cast<const int16_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 32768.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
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
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_S32LE: {
        auto* data = reinterpret_cast<const int32_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs(data[i] * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    case AudioEncoding::PCM_U8: {
        auto* data = reinterpret_cast<const uint8_t*>(audio_buffer.data());
        constexpr float scale = 1.0f / 128.0f;
        for (size_t i = 0; i < total_samples; i += step * format.channels) {
            max_peak = (std::max)(max_peak, std::abs((data[i] - 128) * scale));
            if (++processed_samples >= MAX_SAMPLES)
                break;
        }
        break;
    }
    default:
        return 0.0f;
    }

    return max_peak;
}

audio_playback::AudioEncoding audio_playback_windows::get_AudioEncoding_from_WAVEFORMAT(WAVEFORMATEX* wfx)
{
    if (!wfx)
        return AudioEncoding::INVALID;

    // 基本格式判断
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return (wfx->wBitsPerSample == 32) ? AudioEncoding::PCM_F32LE : AudioEncoding::INVALID;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        switch (wfx->wBitsPerSample) {
        case 8:
            return AudioEncoding::PCM_U8;
        case 16:
            return AudioEncoding::PCM_S16LE;
        case 24:
            return AudioEncoding::PCM_S24LE;
        case 32:
            return AudioEncoding::PCM_S32LE;
        default:
            return AudioEncoding::INVALID;
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
            case 8:
                return AudioEncoding::PCM_U8;
            case 16:
                return AudioEncoding::PCM_S16LE;
            case 24:
                return AudioEncoding::PCM_S24LE;
            case 32:
                return AudioEncoding::PCM_S32LE;
            default:
                return AudioEncoding::INVALID;
            }
        }
    }

    return AudioEncoding::INVALID;
}

bool audio_playback_windows::convert_AudioFormat_to_WAVEFORMAT(const AudioFormat& format, WAVEFORMATEX** pp_wave_format)
{
    if (!pp_wave_format) {
        return false;
    }

    WORD format_tag = 0;
    WORD bits_per_sample = static_cast<WORD>(format.bit_depth);
    bool is_float = AudioFormat::is_float_encoding(format.encoding).value_or(false);

    // 确定格式标签
    if (is_float) {
        format_tag = WAVE_FORMAT_IEEE_FLOAT;
    } else {
        format_tag = WAVE_FORMAT_PCM;
    }

    // 分配内存
    WAVEFORMATEX* p_format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!p_format) {
        return false;
    }

    // 填充结构
    p_format->wFormatTag = format_tag;
    p_format->nChannels = static_cast<WORD>(format.channels);
    p_format->nSamplesPerSec = format.sample_rate;
    p_format->wBitsPerSample = bits_per_sample;
    p_format->nBlockAlign = (p_format->nChannels * p_format->wBitsPerSample) / 8;
    p_format->nAvgBytesPerSec = p_format->nSamplesPerSec * p_format->nBlockAlign;
    p_format->cbSize = 0;

    *pp_wave_format = p_format;
    return true;
}

// 实现DeviceNotifier类
audio_playback_windows::DeviceNotifier::DeviceNotifier(audio_playback_windows* parent)
    : m_parent(parent) {}

ULONG STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::AddRef()
{
    return m_ref_count.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::Release()
{
    ULONG ref = m_ref_count.fetch_sub(1) - 1;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::QueryInterface(REFIID riid, void** ppvObject)
{
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
    spdlog::info("[audio_playback] Device state changed.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_playback] Device added.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_playback] Device removed.");
    m_parent->m_device_changed.store(true);
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)
{
    if (flow == eRender && role == eConsole) {
        spdlog::info("[audio_playback] Default device changed.");
        m_parent->m_device_changed.store(true);
        m_parent->m_device_change_cv.notify_one();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnPropertyValueChanged(
    LPCWSTR pwstrDeviceId, const PROPERTYKEY key)
{
    // 不再检查特定的键值，而是简单记录属性变化但不触发重启
    spdlog::debug("[audio_playback] Device property value changed.");
    return S_OK;
}

bool audio_playback_windows::register_device_notifications()
{
    m_device_notifier = new DeviceNotifier(this);
    HRESULT hr = p_enumerator->RegisterEndpointNotificationCallback(m_device_notifier);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] RegisterEndpointNotificationCallback failed: HRESULT {0:x}", hr);
        delete m_device_notifier;
        m_device_notifier = nullptr;
        return false;
    }
    return true;
}

void audio_playback_windows::unregister_device_notifications()
{
    if (m_device_notifier) {
        p_enumerator->UnregisterEndpointNotificationCallback(m_device_notifier);
        m_device_notifier->Release();
        m_device_notifier = nullptr;
    }
}

void audio_playback_windows::handle_device_change()
{
    spdlog::info("[audio_playback] Handling device change.");

    // 停止当前播放
    if (m_is_playing) {
        spdlog::debug("[audio_playback] Stopping current playback.");
        if (stop_playback()) {
            spdlog::info("[audio_playback] Playback stopped.");
        } else {
            spdlog::error("[audio_playback] Failed to stop playback.");
            throw std::runtime_error("[audio_playback] Stop playback failed.");
        }
    }

    // 重新获取默认设备
    spdlog::debug("[audio_playback] Acquiring new default audio endpoint.");
    p_device.Reset();
    HRESULT hr = p_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_device);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to get default audio endpoint after device change: HRESULT {0:x}", hr);
        return;
    }

    // 重新设置流
    spdlog::debug("[audio_playback] Setting up new audio stream.");
    if (!setup_stream(m_stream_config)) {
        spdlog::error("[audio_playback] Failed to setup stream after device change.");
        return;
    }

    // 重新开始播放
    spdlog::debug("[audio_playback] Restarting playback.");
    if (!start_playback()) {
        spdlog::error("[audio_playback] Failed to restart playback after device change.");
        return;
    }

    spdlog::info("[audio_playback] Device change handled successfully.");
}

void audio_playback_windows::start_device_change_listener()
{
    m_device_change_thread = std::thread([this]() {
        // 初始化设备更改线程的COM为STA
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            spdlog::error("[audio_playback] Device change thread COM init failed: {0:x}", hr);
            return;
        }

        std::unique_lock<std::mutex> lock(m_device_change_mutex);
        while (true) {
            m_device_change_cv.wait(lock, [this]() {
                return m_device_changed.load() || m_device_changed_thread_exit_flag.load();
            });

            if (m_device_changed_thread_exit_flag.load()) {
                spdlog::debug("[audio_playback] Device change listener thread exiting.");
                break;
            }

            if (m_device_changed.load()) {
                m_device_changed.store(false);

                // 在调用 handle_device_change() 之前解锁，避免死锁
                lock.unlock();
                try {
                    handle_device_change();
                } catch (const std::exception& e) {
                    spdlog::error("[audio_playback] Exception in device change handling: {}", e.what());
                }
                lock.lock();
            }
        }

        CoUninitialize();
    });
}

void audio_playback_windows::stop_device_change_listener()
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