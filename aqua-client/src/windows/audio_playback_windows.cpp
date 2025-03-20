//
// Created by QU on 25-2-10.
//

#include "audio_playback_windows.h"

#include <array>
#include <chrono>

#if defined(_WIN32) || defined(_WIN64)

using namespace std::literals::chrono_literals;

audio_playback_windows::audio_playback_windows()
{
    m_stream_config = {
        .encoding = AudioEncoding::PCM_F32LE,
        .channels = m_wave_format.nChannels,
        .sample_rate = m_wave_format.nSamplesPerSec,
        .bit_depth = m_wave_format.wBitsPerSample
    };

    start_device_change_listener();
    spdlog::debug("[audio_playback] Audio manager instance created.");
}

audio_playback_windows::~audio_playback_windows()
{
    stop_device_change_listener();
    stop_playback();

    unregister_device_notifications();

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
    spdlog::info("[audio_playback] Audio manager destroyed.");

}

// 初始化COM库并获取默认音频设备
bool audio_playback_windows::init()
{
    // 初始化设备更改线程的COM为STA
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
    // 保存传入的格式（即使不能直接使用）
    m_stream_config = format;
    
    HRESULT hr = S_OK;
    spdlog::debug("[audio_playback] Entering setup_stream().");

    // 释放之前的资源（如果有）
    if (p_audio_client) {
        hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("[audio_playback] Failed to stop previous audio client: HRESULT {0:x}", hr);
        }
    }
    p_render_client.Reset();
    p_audio_client.Reset();
    spdlog::debug("[audio_playback] Activating audio client.");

    hr = p_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &p_audio_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to activate audio client: HRESULT {0:x}", hr);
        return false;
    }

    REFERENCE_TIME buffer_duration = 20 * 1000; // 20ms
    spdlog::debug("[audio_playback] Initializing audio client.");

    hr = p_audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buffer_duration,
        0,
        &m_wave_format,
        nullptr);

    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Audio client initialization failed: HRESULT {0:x}", hr);
        return false;
    }

    // 创建事件对象
    if (!m_hRenderEvent) {
        m_hRenderEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_hRenderEvent) {
            spdlog::error("CreateEvent failed: {}", GetLastError());
            return false;
        }
    }

    // 设置事件句柄
    hr = p_audio_client->SetEventHandle(m_hRenderEvent);
    if (FAILED(hr)) {
        spdlog::error("[audio_manager] SetEventHandle failed: HRESULT {0:x}", hr);
        return false;
    }

    // 获取播放客户端接口
    spdlog::debug("[audio_playback] Getting playback client.");
    hr = p_audio_client->GetService(__uuidof(IAudioRenderClient), &p_render_client);

    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to get playback client: HRESULT {0:x}", hr);
        return false;
    }

    // 记录当前实际使用的格式（Windows WASAPI使用固定格式）
    // 这里使用的是Windows内部固定格式，与传入的format可能不同
    m_stream_config = {
        .encoding = AudioEncoding::PCM_F32LE,
        .channels = m_wave_format.nChannels,
        .sample_rate = m_wave_format.nSamplesPerSec,
        .bit_depth = m_wave_format.wBitsPerSample
    };
    
    spdlog::info("[audio_playback] Stream configured. Requested format: {}Hz, {}ch, {}bit, Actual format: {}Hz, {}ch, {}bit",
        format.sample_rate, format.channels, format.bit_depth,
        m_stream_config.sample_rate, m_stream_config.channels, m_stream_config.bit_depth);
    
    return SUCCEEDED(hr);
}

bool audio_playback_windows::reconfigure_stream(const AudioFormat& new_format)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 仅当关键格式参数变化时才需要重配置
    if (m_stream_config.channels == new_format.channels &&
        m_stream_config.sample_rate == new_format.sample_rate &&
        m_stream_config.encoding == new_format.encoding) {
        spdlog::debug("[audio_playback] Format unchanged, skipping reconfiguration");
        return true;
    }
    
    spdlog::info("[audio_playback] Reconfiguring stream from {}Hz, {}ch to {}Hz, {}ch",
        m_stream_config.sample_rate, m_stream_config.channels,
        new_format.sample_rate, new_format.channels);
    
    // 保存新格式供转换使用
    m_source_format = new_format;
    
    // Windows不需要重新配置流，因为我们总是使用固定格式
    // 但我们需要更新源格式以便正确转换
    
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

    // 启动捕获线程
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
    return true;;
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

void audio_playback_windows::set_format(AudioFormat format)
{
    m_stream_config = format;
}

bool audio_playback_windows::push_packet_data(std::span<const std::byte> packet_data)
{
    if (packet_data.empty()) {
        spdlog::warn("[audio_playback] Empty packet data received");
        return false;
    }
    
    // 将 std::span<const std::byte> 转换为 std::vector<uint8_t>
    std::vector<uint8_t> data_vec(packet_data.size());
    std::memcpy(data_vec.data(), packet_data.data(), packet_data.size());
    
    // 如果源格式与目标格式不同，执行转换
    if (m_source_format.encoding != m_stream_config.encoding ||
        m_source_format.channels != m_stream_config.channels ||
        m_source_format.sample_rate != m_stream_config.sample_rate) {
        
        std::vector<float> converted_data;
        if (convert_audio_data(data_vec, m_source_format, converted_data)) {
            // 转换后的数据是float格式的，直接转换回字节数组推送到缓冲区
            data_vec.resize(converted_data.size() * sizeof(float));
            std::memcpy(data_vec.data(), converted_data.data(), data_vec.size());
        }
        else {
            spdlog::error("[audio_playback] Format conversion failed");
            return false;
        }
    }
    
    return m_adaptive_buffer.push_buffer_packets(std::move(data_vec));
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

    const UINT32 channels = m_stream_config.channels;
    std::vector<float> buffer(buffer_total_frames * channels);

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
            const size_t needed_bytes = available_frames * channels * sizeof(float);
            
            // 创建临时缓冲区
            std::vector<uint8_t> temp_buffer(needed_bytes);
            
            // 从自适应缓冲区获取字节数据
            const size_t filled_bytes = m_adaptive_buffer.pull_buffer_data(
                temp_buffer.data(), needed_bytes);

            if (filled_bytes > 0) {
                // 将字节数据复制到音频缓冲区
                std::memcpy(p_audio_data, temp_buffer.data(), filled_bytes);
                
                // 如果需要，填充剩余部分为静音
                if (filled_bytes < needed_bytes) {
                    std::memset(p_audio_data + filled_bytes, 0, needed_bytes - filled_bytes);
                }
                
                // 计算填充的帧数
                const UINT32 filled_frames = available_frames;
                p_render_client->ReleaseBuffer(filled_frames, 0);
                
                // 处理音频数据以计算音量峰值
                std::span<const std::byte> audio_span(
                    reinterpret_cast<const std::byte*>(temp_buffer.data()), 
                    filled_bytes);
                process_audio_buffer(audio_span);
            } else {
                // 填充静音
                std::memset(p_audio_data, 0, needed_bytes);
                p_render_client->ReleaseBuffer(available_frames, 0);
            }
        } else if (waitResult == WAIT_FAILED) {
            spdlog::error("[audio_playback] WaitForSingleObject failed: {}", GetLastError());
            break;
        }
    }
}

void audio_playback_windows::process_audio_buffer(std::span<const std::byte> audio_buffer)
{
    if (audio_buffer.empty())
        return;

    // 峰值计算
    if (!m_peak_callback)
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

// 添加格式转换实现
bool audio_playback_windows::convert_audio_data(
    std::vector<uint8_t>& input_data,
    const AudioFormat& input_format,
    std::vector<float>& output_data)
{
    // 简单的格式转换实现
    // 注意：这是一个基本实现，可能需要更复杂的处理
    
    // 这里仅实现几种常见的转换：
    // 1. PCM_S16LE -> PCM_F32LE
    // 2. 通道数转换（单声道<->立体声）
    // 3. 采样率转换暂不实现，需要更复杂的算法或第三方库
    
    if (input_data.empty()) {
        return false;
    }
    
    // 根据源格式计算样本数
    size_t num_samples = input_data.size() / (input_format.bit_depth / 8) / input_format.channels;
    
    // 预分配输出缓冲区
    output_data.resize(num_samples * m_stream_config.channels);
    
    // 根据编码类型进行转换
    switch (input_format.encoding) {
        case AudioEncoding::PCM_S16LE:
            {
                // 16位整型转浮点
                const int16_t* src = reinterpret_cast<const int16_t*>(input_data.data());
                for (size_t i = 0; i < num_samples; i++) {
                    for (size_t ch = 0; ch < m_stream_config.channels; ch++) {
                        // 如果源通道数少于目标通道数，则复制最后一个通道
                        size_t src_ch = std::min(ch, input_format.channels - 1);
                        output_data[i * m_stream_config.channels + ch] = 
                            src[i * input_format.channels + src_ch] / 32768.0f;
                    }
                }
            }
            break;
            
        case AudioEncoding::PCM_F32LE:
            {
                // 浮点到浮点，可能只需要处理通道数
                const float* src = reinterpret_cast<const float*>(input_data.data());
                for (size_t i = 0; i < num_samples; i++) {
                    for (size_t ch = 0; ch < m_stream_config.channels; ch++) {
                        size_t src_ch = std::min(ch, input_format.channels - 1);
                        output_data[i * m_stream_config.channels + ch] = 
                            src[i * input_format.channels + src_ch];
                    }
                }
            }
            break;
            
        // 其他格式转换可以按需添加
            
        default:
            spdlog::error("[audio_playback] Unsupported source format: {}", 
                         static_cast<int>(input_format.encoding));
            return false;
    }
    
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
    {
        std::lock_guard<std::mutex> lock(m_parent->m_device_change_mutex);
        m_parent->m_device_changed.store(true);
    }
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDeviceAdded(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_playback] Device added.");
    {
        std::lock_guard<std::mutex> lock(m_parent->m_device_change_mutex);
        m_parent->m_device_changed.store(true);
    }
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDeviceRemoved(LPCWSTR pwstrDeviceId)
{
    spdlog::info("[audio_playback] Device removed.");
    {
        std::lock_guard<std::mutex> lock(m_parent->m_device_change_mutex);
        m_parent->m_device_changed.store(true);
    }
    m_parent->m_device_change_cv.notify_one();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)
{
    if (flow == eRender && role == eConsole) {
        spdlog::info("[audio_playback] Default device changed.");

        // 设置设备已更改标志
        {
            std::lock_guard<std::mutex> lock(m_parent->m_device_change_mutex);
            m_parent->m_device_changed.store(true);
        }
        m_parent->m_device_change_cv.notify_one();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE audio_playback_windows::DeviceNotifier::OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key)
{
    spdlog::info("[audio_playback] Device property value changed.");
    {
        std::lock_guard<std::mutex> lock(m_parent->m_device_change_mutex);
        m_parent->m_device_changed.store(true);
    }
    m_parent->m_device_change_cv.notify_one();
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

    // 停止当前捕获
    if (m_is_playing) {
        spdlog::debug("[audio_manager] Stopping current capture.");
        if (stop_playback()) {
            spdlog::info("[audio_manager] Capture stopped.");
        } else {
            spdlog::error("[audio_manager] Capture failed.");
            throw std::runtime_error("[audio_manager] Stop capture failed.");
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
        // TODO: notify parent level to handle exception.
        return;
    }

    // 重新开始捕获，使用保存的用户回调函数
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
            spdlog::error("[audio_manager] Device change thread COM init failed: {0:x}", hr);
            return;
        }

        std::unique_lock<std::mutex> lock(m_device_change_mutex);
        while (true) {
            m_device_change_cv.wait(lock, [this]() { return m_device_changed.load() || m_device_changed_thread_exit_flag.load(); });

            if (m_device_changed_thread_exit_flag.load()) {
                spdlog::debug("[audio_playback] Device change listener thread exiting.");
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
#endif // _WIN32 || _WIN64