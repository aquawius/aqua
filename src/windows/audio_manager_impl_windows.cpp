//
// Created by aquawius on 25-1-11.
//

#include "audio_manager_impl_windows.h"

#include <array>

#if defined(_WIN32) || defined(_WIN64)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")

// 构造函数：初始化成员变量
audio_manager_impl::audio_manager_impl()
{
    spdlog::debug("Audio manager instance created.");
}

// 析构函数：资源清理
audio_manager_impl::~audio_manager_impl()
{
    stop_capture();

    // 释放音频格式内存
    if (p_wave_format) {
        CoTaskMemFree(p_wave_format);
        p_wave_format = nullptr;
        spdlog::debug("Wave format released.");
    }

    // 释放COM接口
    p_capture_client.Reset();
    p_audio_client.Reset();
    p_device.Reset();

    // 关闭事件句柄
    if (h_capture_event) {
        CloseHandle(h_capture_event);
        h_capture_event = nullptr;
        spdlog::debug("Capture event handle closed.");
    }

    // 反初始化COM库
    CoUninitialize();
    spdlog::info("Audio manager destroyed.");
}

// 初始化COM库并获取默认音频设备
bool audio_manager_impl::init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        spdlog::error("COM initialization failed: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("COM library initialized.");

    // 创建设备枚举器
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        spdlog::error("Failed to create device enumerator: HRESULT {0:x}", hr);
        return false;
    }

    // 获取默认音频输出设备
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_device);
    if (FAILED(hr)) {
        spdlog::error("Failed to get default audio endpoint: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("Default audio endpoint acquired.");
    return true;
}

// 配置音频流参数并初始化客户端
bool audio_manager_impl::setup_stream()
{
    HRESULT hr = p_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &p_audio_client);
    if (FAILED(hr)) {
        spdlog::error("Failed to activate audio client: HRESULT {0:x}", hr);
        return false;
    }

    // 获取音频混合格式
    hr = p_audio_client->GetMixFormat(&p_wave_format);
    if (FAILED(hr)) {
        spdlog::error("Failed to get mix format: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("Audio format: {} Hz, {} channels, {} bits/sample",
        p_wave_format->nSamplesPerSec,
        p_wave_format->nChannels,
        p_wave_format->wBitsPerSample);

    // 初始化音频客户端（启用事件回调）
    constexpr REFERENCE_TIME buffer_duration = 250000; // 25ms 音频系统内部缓冲区的容量
    hr = p_audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        // 坑, 要在音频客户端中使用事件驱动模式, 必须在初始化时包含`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`标志
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buffer_duration,
        0,
        p_wave_format,
        nullptr);

    if (FAILED(hr)) {
        spdlog::error("Audio client initialization failed: HRESULT {0:x}", hr);
        return false;
    }

    // 获取捕获客户端接口
    hr = p_audio_client->GetService(__uuidof(IAudioCaptureClient), &p_capture_client);
    if (FAILED(hr)) {
        spdlog::error("Failed to get capture client: HRESULT {0:x}", hr);
        return false;
    }

    // 创建事件句柄用于缓冲区通知
    h_capture_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!h_capture_event) {
        spdlog::error("Failed to create capture event handle.");
        return false;
    }

    // 绑定事件到音频客户端
    hr = p_audio_client->SetEventHandle(h_capture_event);
    if (FAILED(hr)) {
        spdlog::error("Failed to set event handle: HRESULT {0:x}", hr);
        return false;
    }

    // 更新流配置信息
    m_stream_config.rate = p_wave_format->nSamplesPerSec;
    m_stream_config.channels = p_wave_format->nChannels;
    spdlog::info("Audio stream configured: {} Hz, {} channels",
        m_stream_config.rate, m_stream_config.channels);
    return true;
}

// 启动音频捕获线程
bool audio_manager_impl::start_capture(AudioDataCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_capturing) {
        spdlog::warn("Capture already running. Ignoring start request.");
        return false;
    }

    m_data_callback = std::move(callback);
    m_promise_initialized = std::promise<void>();

    // 启动音频客户端
    HRESULT hr = p_audio_client->Start();
    if (FAILED(hr)) {
        spdlog::error("Failed to start audio client: HRESULT {0:x}", hr);
        return false;
    }
    spdlog::info("Audio client started.");

    // 启动捕获线程
    m_capture_thread = std::jthread([this](std::stop_token stop_token) {
        m_is_capturing = true;
        m_promise_initialized.set_value();
        spdlog::info("Capture thread started.");
        capture_thread_loop(stop_token);
        m_is_capturing = false;
        spdlog::info("Capture thread stopped.");
    });

    // 等待线程初始化完成
    m_promise_initialized.get_future().wait();
    return true;
}

// 停止捕获并清理资源
bool audio_manager_impl::stop_capture() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_capturing) {
        spdlog::warn("No active capture to stop.");
        return false;
    }

    // 停止音频流并唤醒线程
    if (p_audio_client) {
        HRESULT hr = p_audio_client->Stop();
        if (FAILED(hr)) {
            spdlog::error("Failed to stop audio client: HRESULT {0:x}", hr);
        }
    }

    // 强制唤醒等待线程
    if (h_capture_event) {
        SetEvent(h_capture_event);
    }

    // 请求线程停止并等待退出
    m_capture_thread.request_stop();
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
        spdlog::debug("Capture thread joined.");
    }

    // 关闭事件句柄
    if (h_capture_event) {
        CloseHandle(h_capture_event);
        h_capture_event = nullptr;
    }

    m_is_capturing = false;
    return true;
}

// 查询当前捕获状态
bool audio_manager_impl::is_capturing() const
{
    return m_is_capturing.load();
}

// 获取当前音频流配置
const audio_manager_impl::stream_config& audio_manager_impl::get_format() const
{
    return m_stream_config;
}

// PCM格式转换实现
void audio_manager_impl::convert_pcm16_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples)
{
    const int16_t* src = reinterpret_cast<const int16_t*>(pData);
    for (UINT32 i = 0; i < numSamples; ++i) {
        buffer[i] = static_cast<float>(src[i]) / 32768.0f; // 16位有符号转浮点
    }
}

void audio_manager_impl::convert_pcm24_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples)
{
    for (UINT32 i = 0; i < numSamples; ++i) {
        const BYTE* sample = pData + i * 3;
        int32_t value = (static_cast<int32_t>(sample[0]) << 8) | (static_cast<int32_t>(sample[1]) << 16) | (static_cast<int32_t>(sample[2]) << 24);
        value >>= 8; // 符号扩展
        buffer[i] = static_cast<float>(value) / 8388608.0f; // 24位有符号转浮点
    }
}

void audio_manager_impl::convert_pcm32_to_float(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples)
{
    const int32_t* src = reinterpret_cast<const int32_t*>(pData);
    for (UINT32 i = 0; i < numSamples; ++i) {
        buffer[i] = static_cast<float>(src[i]) / 2147483648.0f; // 32位有符号转浮点
    }
}

// 统一音频格式处理入口
void audio_manager_impl::handle_format_conversion(const BYTE* pData, std::vector<float>& buffer, UINT32 numSamples)
{
    if (p_wave_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        // 直接拷贝浮点数据
        memcpy(buffer.data(), pData, numSamples * sizeof(float));
    } else if (p_wave_format->wFormatTag == WAVE_FORMAT_PCM) {
        // 根据位深选择转换函数
        switch (p_wave_format->wBitsPerSample) {
        case 16:
            convert_pcm16_to_float(pData, buffer, numSamples);
            break;
        case 24:
            convert_pcm24_to_float(pData, buffer, numSamples);
            break;
        case 32:
            convert_pcm32_to_float(pData, buffer, numSamples);
            break;
        default:
            spdlog::warn("Unsupported bit depth: {}", p_wave_format->wBitsPerSample);
        }
    } else if (p_wave_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // 处理扩展格式（如多声道）
        WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(p_wave_format);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            memcpy(buffer.data(), pData, numSamples * sizeof(float));
        } else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            switch (p_wave_format->wBitsPerSample) {
            case 16:
                convert_pcm16_to_float(pData, buffer, numSamples);
                break;
            case 24:
                convert_pcm24_to_float(pData, buffer, numSamples);
                break;
            case 32:
                convert_pcm32_to_float(pData, buffer, numSamples);
                break;
            default:
                spdlog::warn("Unsupported bit depth: {}", p_wave_format->wBitsPerSample);
            }
        } else {
            spdlog::warn("Unsupported subformat in extensible wave format.");
        }
    } else {
        spdlog::warn("Unsupported format tag: {}", p_wave_format->wFormatTag);
    }
}

// 捕获线程主循环
void audio_manager_impl::capture_thread_loop(std::stop_token stop_token)
{
    thread_local std::vector<float> audio_buffer; // 线程局部缓冲区
    const UINT32 channels = m_stream_config.channels;

    while (!stop_token.stop_requested()) {
        // 等待音频数据就绪事件
        DWORD waitResult = WaitForSingleObject(h_capture_event, 15); // 捕获线程中等待音频数据就绪事件的超时时间
        if (waitResult != WAIT_OBJECT_0) {
            if (waitResult == WAIT_FAILED) {
                spdlog::error("WaitForSingleObject failed: {}", GetLastError());
            }
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT hr = p_capture_client->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            spdlog::error("GetNextPacketSize failed: HRESULT {0:x}", hr);
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
                spdlog::error("GetBuffer failed: HRESULT {0:x}", hr);
                p_audio_client->Stop(); // 确保停止音频流
                break;
            }

            if (numFrames == 0) {
                spdlog::debug("Empty buffer received.");
                continue;
            }

            // 转换数据格式并填充缓冲区
            const UINT32 numSamples = numFrames * channels;
            audio_buffer.resize(numSamples);
            handle_format_conversion(pData, audio_buffer, numSamples);

            if (!audio_buffer.empty() && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                process_audio_buffer(audio_buffer);
            } else {
                spdlog::debug("Silent buffer skipped.");
            }

            // 释放缓冲区并获取下一个包大小
            hr = p_capture_client->ReleaseBuffer(numFrames);
            if (FAILED(hr)) {
                spdlog::error("ReleaseBuffer failed: HRESULT {0:x}", hr);
                break;
            }

            hr = p_capture_client->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                spdlog::error("GetNextPacketSize failed: HRESULT {0:x}", hr);
                break;
            }
        }
    }
}

void audio_manager_impl::process_audio_buffer(const std::span<const float> audio_buffer) const
{
    if (audio_buffer.empty())
        return;

    display_volume(audio_buffer);

    if (m_data_callback) {
        m_data_callback(audio_buffer);
        // spdlog::trace("[Windows] Sent {} samples to callback.", data.size());
    } else {
        spdlog::warn("[Windows] No callback set.");
    }
}

void audio_manager_impl::display_volume(const std::span<const float> data) const
{
    if (spdlog::get_level() > spdlog::level::debug || data.empty()) {
        return;
    }

    constexpr size_t METER_WIDTH = 40;
    static std::array<char, METER_WIDTH + 1> meter_buffer;
    meter_buffer.fill('-');

    const size_t size = data.size();

    // 采样几个关键点计算最大值
    float local_peak = std::abs(data[0]); // 起始点
    // fix windows.h defined marco `max`
    local_peak = (std::max)(local_peak, std::abs(data[size - 1])); // 终点
    local_peak = (std::max)(local_peak, std::abs(data[size / 2])); // 中点
    local_peak = (std::max)(local_peak, std::abs(data[size / 4])); // 1/4点
    local_peak = (std::max)(local_peak, std::abs(data[size * 3 / 4])); // 3/4点
    local_peak = (std::max)(local_peak, std::abs(data[size / 8])); // 1/8点
    local_peak = (std::max)(local_peak, std::abs(data[size * 7 / 8])); // 7/8点

    // 计算峰值电平并更新音量条
    const int peak_level = std::clamp(static_cast<int>(local_peak * METER_WIDTH), 0,
        static_cast<int>(METER_WIDTH));

    if (peak_level > 0) {
        std::fill_n(meter_buffer.begin(), peak_level, '#');
    }

    meter_buffer[METER_WIDTH] = '\0';
    spdlog::debug("[{}] {:.3f}", meter_buffer.data(), local_peak);
}

#endif // defined(_WIN32) || defined(_WIN64)