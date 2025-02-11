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
        m_wave_format.nSamplesPerSec,
        m_wave_format.nChannels,
        512
    };

    start_device_change_listener();
    spdlog::debug("[audio_playback] Audio manager instance created.");
}

audio_playback_windows::~audio_playback_windows()
{
    stop_device_change_listener();
    stop_playback();

    unregister_device_notifications();

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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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

bool audio_playback_windows::setup_stream()
{
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

    hr = p_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &p_audio_client);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to activate audio client: HRESULT {0:x}", hr);
        return false;
    }

    REFERENCE_TIME buffer_duration = 1000000; // 100ms
    spdlog::debug("[audio_playback] Initializing audio client.");

    hr = p_audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buffer_duration, 0, &m_wave_format, nullptr);
    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Audio client initialization failed: HRESULT {0:x}", hr);
        return false;
    }

    // 获取播放客户端接口
    spdlog::debug("[audio_playback] Getting playback client.");
    hr = p_audio_client->GetService(__uuidof(IAudioRenderClient), &p_render_client);

    if (FAILED(hr)) {
        spdlog::error("[audio_playback] Failed to get playback client: HRESULT {0:x}", hr);
        return false;
    }

    return SUCCEEDED(hr);
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

const audio_playback_windows::stream_config& audio_playback_windows::get_format() const
{
    return m_stream_config;
}

bool audio_playback_windows::push_packet_data(const std::vector<uint8_t>& origin_packet_data)
{
    return m_adaptive_buffer.push_buffer_packets(std::vector<uint8_t>(origin_packet_data));
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
        // 获取当前可写入的帧数
        UINT32 padding_frames = 0;
        hr = p_audio_client->GetCurrentPadding(&padding_frames);
        if (FAILED(hr)) {
            spdlog::warn("[audio_playback] GetCurrentPadding failed: HRESULT {0:x}", hr);
            continue;
        }

        const UINT32 available_frames = buffer_total_frames - padding_frames;
        if (available_frames == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        // 申请可写入的缓冲区
        BYTE* p_audio_data = nullptr;
        hr = p_render_client->GetBuffer(available_frames, &p_audio_data);
        if (FAILED(hr)) {
            spdlog::warn("[audio_playback] GetBuffer failed: HRESULT {0:x}", hr);
            continue;
        }

        // 从自适应缓冲区拉取数据
        const size_t requested_samples = available_frames * channels;
        const size_t filled_samples = m_adaptive_buffer.pull_buffer_data(
            buffer.data(), requested_samples);

        if (filled_samples > 0) {
            // 计算实际填充的帧数（样本数 / 声道数）
            const UINT32 filled_frames = static_cast<UINT32>(filled_samples / channels);

            // 复制数据到音频缓冲区
            memcpy(p_audio_data, buffer.data(), filled_samples * sizeof(float));

            // 仅提交实际填充的帧数
            p_render_client->ReleaseBuffer(filled_frames, 0);

            display_volume({ buffer.data(), filled_samples });
        } else {
            // 无有效数据时填充静音
            memset(p_audio_data, 0, available_frames * channels * sizeof(float));
            p_render_client->ReleaseBuffer(available_frames, 0);
            spdlog::trace("[audio_playback] Filled silence ({} frames)", available_frames);
        }
    }
}

void audio_playback_windows::display_volume(const std::span<const float> data) const
{
    if (spdlog::get_level() > spdlog::level::debug || data.empty()) {
        return;
    }

    constexpr size_t METER_WIDTH = 40;
    static std::array<char, METER_WIDTH + 1> meter_buffer;
    meter_buffer.fill('-');

    const size_t size = data.size();

    // 采样几个关键点计算最大值
    // fix windows.h defined marco `max`
    float local_peak = (std::abs)(data[0]); // 起始点
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
    if (!setup_stream()) {
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