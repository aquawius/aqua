#include "core/audio/backend/wasapi/wasapi_capture.h"

#include "core/audio/backend/wasapi/wasapi_common.h"
#include "core/logger/logger.h"

#include <chrono>
#include <thread>

namespace aqua::audio {

namespace {
using wasapi::ComPtr;
using wasapi::wave_format_to_audio_format;
} // namespace

WasapiCapture::~WasapiCapture()
{
    stop();
}

bool WasapiCapture::start(CaptureCallback cb, AudioFormat& out_format)
{
    if (running_) return false;
    callback_ = std::move(cb);
    running_ = true;
    started_ = false;
    thread_ = std::thread(&WasapiCapture::capture_loop, this);

    // 等待线程初始化结果（最多 1 秒）。
    // WASAPI 初始化（CoCreateInstance/Activate/GetMixFormat/Initialize/GetService/Start）
    // 通常 < 100ms，失败会很快返回并置 running_=false；成功会置 started_=true。
    // 与 WasapiPlayback::start() 等待 started_ 的模式一致。
    for (int i = 0; i < 100 && running_.load(std::memory_order_acquire) && !started_.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!started_.load(std::memory_order_acquire)) {
        // 初始化失败或超时：join 线程并返回 false，让调用方走错误清理路径。
        // 此前日志已由 capture_loop 输出具体 HRESULT。
        stop();
        return false;
    }

    out_format = format_;
    return true;
}

void WasapiCapture::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    callback_ = {};
    started_ = false;
}

bool WasapiCapture::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

void WasapiCapture::capture_loop()
{
    // 提高 Windows 定时器分辨率到 1ms，使 sleep_for(1ms) 实际生效（H6）。
    wasapi::WindowsTimerResolution timer_res;

    // COM 初始化（MTA）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        log_error_fmt("WASAPI capture: CoInitializeEx failed: 0x{:08X}", static_cast<unsigned>(hr));
        running_ = false;
        return;
    }

    // 获取默认渲染设备（loopback 目标）
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: CoCreateInstance enumerator failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: GetDefaultAudioEndpoint failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    ComPtr<IAudioClient> audio_client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(audio_client.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: Activate IAudioClient failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    // stop() 可能在初始化期间被调用（如 start() 超时），及时退出避免无界等待（H7）
    if (!running_.load(std::memory_order_acquire)) {
        if (com_initialized) CoUninitialize();
        return;
    }

    // 获取 mix format
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        log_error_fmt("WASAPI capture: GetMixFormat failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (mix_format) CoTaskMemFree(mix_format);
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    // stop() 可能在初始化期间被调用（如 start() 超时），及时退出避免无界等待（H7）
    if (!running_.load(std::memory_order_acquire)) {
        CoTaskMemFree(mix_format);
        if (com_initialized) CoUninitialize();
        return;
    }

    auto fmt_opt = wave_format_to_audio_format(mix_format);
    if (!fmt_opt) {
        log_error("WASAPI capture: unsupported mix format");
        CoTaskMemFree(mix_format);
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }
    format_ = *fmt_opt;

    // 初始化 audio client — loopback 模式
    // buffer duration: 20ms (单位 100ns)
    constexpr REFERENCE_TIME BUFFER_DURATION = 200000;
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  BUFFER_DURATION, 0, mix_format, nullptr);
    CoTaskMemFree(mix_format);
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: Initialize failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    // stop() 可能在初始化期间被调用（如 start() 超时），及时退出避免无界等待（H7）
    if (!running_.load(std::memory_order_acquire)) {
        if (com_initialized) CoUninitialize();
        return;
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(IID_PPV_ARGS(capture_client.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: GetService capture failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    // stop() 可能在初始化期间被调用（如 start() 超时），及时退出避免无界等待（H7）
    if (!running_.load(std::memory_order_acquire)) {
        if (com_initialized) CoUninitialize();
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: Start failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    // stop() 可能在初始化期间被调用（如 start() 超时），及时退出避免无界等待（H7）
    if (!running_.load(std::memory_order_acquire)) {
        audio_client->Stop();
        if (com_initialized) CoUninitialize();
        return;
    }

    log_info_fmt("WASAPI capture started: {}ch {}Hz encoding={}",
                 format_.channels, format_.sample_rate, static_cast<int>(format_.encoding));

    // 诊断：设备周期和缓冲区大小
    {
        REFERENCE_TIME default_period = 0, min_period = 0;
        if (SUCCEEDED(audio_client->GetDevicePeriod(&default_period, &min_period))) {
            log_info_fmt("WASAPI capture device period: default={:.2f}ms min={:.2f}ms",
                         default_period / 10000.0, min_period / 10000.0);
        }
        std::uint32_t buf_frames = 0;
        if (SUCCEEDED(audio_client->GetBufferSize(&buf_frames))) {
            log_info_fmt("WASAPI capture buffer: {} frames ({:.2f}ms)",
                         buf_frames, buf_frames * 1000.0 / format_.sample_rate);
        }
    }

    // 初始化全部成功：通知 start() 可以返回 true。
    started_.store(true, std::memory_order_release);

    const std::uint32_t frame_bytes = format_.frame_bytes();

    // 周期性统计日志（每 5 秒输出一次，便于观察采集流量而不刷屏）
    constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
    auto last_stats_time = std::chrono::steady_clock::now();
    std::uint64_t stats_packets = 0;
    std::uint64_t stats_bytes = 0;

    // 采集循环
    while (running_) {
        try {
            std::uint32_t packet_size = 0;
            hr = capture_client->GetNextPacketSize(&packet_size);
            if (FAILED(hr)) {
                log_warn_fmt("WASAPI capture: GetNextPacketSize failed: 0x{:08X}", static_cast<unsigned>(hr));
                break;
            }

            if (packet_size == 0) {
                // 无数据，短暂休眠
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            BYTE* data = nullptr;
            std::uint32_t num_frames = 0;
            DWORD flags = 0;
            hr = capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                log_warn_fmt("WASAPI capture: GetBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
                break;
            }

            if (num_frames > 0 && data) {
                const std::size_t byte_size = static_cast<std::size_t>(num_frames) * frame_bytes;
                if (callback_) {
                    callback_(std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(data), byte_size});
                }
                ++stats_packets;
                stats_bytes += byte_size;
            }

            hr = capture_client->ReleaseBuffer(num_frames);
            if (FAILED(hr)) {
                log_warn_fmt("WASAPI capture: ReleaseBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
                break;
            }

            // 周期性输出采集统计
            const auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= STATS_INTERVAL) {
                const auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - last_stats_time).count();
                log_debug_fmt("WASAPI capture stats: {} packets, {} bytes in {:.2f}s ({:.1f} packets/s, {:.1f} KB/s)",
                              stats_packets, stats_bytes, secs,
                              static_cast<double>(stats_packets) / secs,
                              static_cast<double>(stats_bytes) / 1024.0 / secs);
                stats_packets = 0;
                stats_bytes = 0;
                last_stats_time = now;
            }
        } catch (const std::exception& e) {
            log_error_fmt("WASAPI capture: callback exception: {}", e.what());
            break;
        } catch (...) {
            log_error("WASAPI capture: unknown callback exception");
            break;
        }
    }

    // 运行时错误（break）或 stop() 请求（running_ 被置 false）都会到达此处。
    // 显式置 running_=false，让 is_running() 正确反映线程已退出，便于主循环检测。
    running_.store(false, std::memory_order_release);
    audio_client->Stop();
    if (com_initialized) CoUninitialize();
    log_info("WASAPI capture stopped");
}

} // namespace aqua::audio
