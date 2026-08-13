#include "core/audio/backend/wasapi/wasapi_playback.h"

#include "core/audio/backend/wasapi/wasapi_common.h"
#include "core/logger/logger.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace aqua::audio {

namespace {
using wasapi::ComPtr;
using wasapi::audio_format_to_wave_format;
} // namespace

WasapiPlayback::~WasapiPlayback()
{
    stop();
}

bool WasapiPlayback::start(AudioFormat format, FillCallback cb)
{
    if (running_) return false;
    if (!format.valid()) return false;

    format_ = format;
    callback_ = std::move(cb);
    running_ = true;
    started_ = false;
    thread_ = std::thread(&WasapiPlayback::playback_loop, this);

    // 等待线程初始化结果（最多 1 秒）。
    // WASAPI 初始化（CoCreateInstance/Activate/Initialize/Start）通常 < 100ms，
    // 失败会很快返回并置 running_=false；成功会置 started_=true。
    // 与 WasapiCapture::start() 等待 format_ 的模式一致。
    for (int i = 0; i < 100 && running_.load(std::memory_order_acquire) && !started_.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!started_.load(std::memory_order_acquire)) {
        // 初始化失败或超时：join 线程并返回 false，让调用方走错误清理路径。
        // 此前日志已由 playback_loop 输出具体 HRESULT。
        stop();
        return false;
    }
    return true;
}

void WasapiPlayback::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    callback_ = {};
    started_ = false;
}

bool WasapiPlayback::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

void WasapiPlayback::playback_loop()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool com_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        log_error_fmt("WASAPI playback: CoInitializeEx failed: 0x{:08X}", static_cast<unsigned>(hr));
        running_ = false;
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: CoCreateInstance failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: GetDefaultAudioEndpoint failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    ComPtr<IAudioClient> audio_client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(audio_client.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: Activate failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    WAVEFORMATEXTENSIBLE wfx{};
    if (!audio_format_to_wave_format(format_, wfx)) {
        log_error("WASAPI playback: unsupported audio format");
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    constexpr REFERENCE_TIME BUFFER_DURATION = 200000; // 20ms
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                  BUFFER_DURATION, 0,
                                  reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: Initialize failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    ComPtr<IAudioRenderClient> render_client;
    hr = audio_client->GetService(IID_PPV_ARGS(render_client.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: GetService render failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    std::uint32_t buffer_frames = 0;
    hr = audio_client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: GetBufferSize failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        log_error_fmt("WASAPI playback: Start failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    log_info_fmt("WASAPI playback started: {}ch {}Hz encoding={}",
                 format_.channels, format_.sample_rate, static_cast<int>(format_.encoding));

    // 诊断：设备周期、缓冲区大小和流延迟
    {
        REFERENCE_TIME default_period = 0, min_period = 0;
        if (SUCCEEDED(audio_client->GetDevicePeriod(&default_period, &min_period))) {
            log_info_fmt("WASAPI playback device period: default={:.2f}ms min={:.2f}ms",
                         default_period / 10000.0, min_period / 10000.0);
        }
        log_info_fmt("WASAPI playback buffer: {} frames ({:.2f}ms)",
                     buffer_frames, buffer_frames * 1000.0 / format_.sample_rate);
        REFERENCE_TIME stream_latency = 0;
        if (SUCCEEDED(audio_client->GetStreamLatency(&stream_latency))) {
            log_info_fmt("WASAPI playback stream latency: {:.2f}ms",
                         stream_latency / 10000.0);
        }
    }

    // 初始化全部成功：通知 start() 可以返回 true。
    started_.store(true, std::memory_order_release);

    const std::uint32_t frame_bytes = format_.frame_bytes();

    // 周期性统计日志（每 5 秒输出一次，便于观察播放流量而不刷屏）
    constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
    auto last_stats_time = std::chrono::steady_clock::now();
    std::uint64_t stats_callbacks = 0;
    std::uint64_t stats_bytes_filled = 0;
    std::uint64_t stats_bytes_silent = 0;

    while (running_) {
        std::uint32_t padding = 0;
        hr = audio_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            log_warn_fmt("WASAPI playback: GetCurrentPadding failed: 0x{:08X}", static_cast<unsigned>(hr));
            break;
        }

        std::uint32_t available = buffer_frames - padding;
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        BYTE* data = nullptr;
        hr = render_client->GetBuffer(available, &data);
        if (FAILED(hr) || !data) {
            log_warn_fmt("WASAPI playback: GetBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
            break;
        }

        const std::size_t bytes_needed = static_cast<std::size_t>(available) * frame_bytes;
        std::size_t filled = 0;
        if (callback_) {
            filled = callback_(std::span<std::byte>{
                reinterpret_cast<std::byte*>(data), bytes_needed});
        }

        // 未填充部分填充静音
        if (filled < bytes_needed) {
            std::memset(reinterpret_cast<std::byte*>(data) + filled, 0, bytes_needed - filled);
        }

        ++stats_callbacks;
        stats_bytes_filled += filled;
        stats_bytes_silent += (bytes_needed - filled);

        std::uint32_t frames_written = static_cast<std::uint32_t>(bytes_needed / frame_bytes);
        hr = render_client->ReleaseBuffer(frames_written, 0);
        if (FAILED(hr)) {
            log_warn_fmt("WASAPI playback: ReleaseBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
            break;
        }

        // 周期性输出播放统计
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= STATS_INTERVAL) {
            const auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - last_stats_time).count();
            const std::uint64_t total = stats_bytes_filled + stats_bytes_silent;
            const double fill_ratio = total > 0
                ? (static_cast<double>(stats_bytes_filled) * 100.0 / static_cast<double>(total))
                : 0.0;
            log_debug_fmt("WASAPI playback stats: {} callbacks, filled {:.1f} KB, silent {:.1f} KB in {:.2f}s (fill ratio {:.1f}%)",
                          stats_callbacks,
                          static_cast<double>(stats_bytes_filled) / 1024.0,
                          static_cast<double>(stats_bytes_silent) / 1024.0,
                          secs, fill_ratio);
            stats_callbacks = 0;
            stats_bytes_filled = 0;
            stats_bytes_silent = 0;
            last_stats_time = now;
        }
    }

    // 运行时错误（break）或 stop() 请求（running_ 被置 false）都会到达此处。
    // 显式置 running_=false，让 is_running() 正确反映线程已退出，便于主循环检测。
    running_.store(false, std::memory_order_release);
    audio_client->Stop();
    if (com_initialized) CoUninitialize();
    log_info("WASAPI playback stopped");
}

} // namespace aqua::audio
