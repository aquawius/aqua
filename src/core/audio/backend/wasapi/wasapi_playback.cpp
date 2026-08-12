#include "core/audio/backend/wasapi/wasapi_playback.h"

#include "core/logger/logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace aqua::audio {

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : ptr_(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : ptr_(o.release()) {}
    ComPtr& operator=(ComPtr&& o) noexcept { reset(o.release()); return *this; }

    void reset(T* p = nullptr) { if (ptr_) ptr_->Release(); ptr_ = p; }
    T* release() { T* t = ptr_; ptr_ = nullptr; return t; }
    T* get() const { return ptr_; }
    T** put() { return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
private:
    T* ptr_ = nullptr;
};

// 将 AudioFormat 转换为 WAVEFORMATEXTENSIBLE
bool audio_format_to_wave_format(const AudioFormat& fmt, WAVEFORMATEXTENSIBLE& wfx) {
    std::memset(&wfx, 0, sizeof(wfx));
    wfx.Format.nChannels = static_cast<WORD>(fmt.channels);
    wfx.Format.nSamplesPerSec = fmt.sample_rate;
    wfx.Format.wBitsPerSample = static_cast<WORD>(fmt.bytes_per_sample() * 8);
    wfx.Format.nBlockAlign = static_cast<WORD>(fmt.frame_bytes());
    wfx.Format.nAvgBytesPerSec = fmt.sample_rate * fmt.frame_bytes();
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    switch (fmt.encoding) {
    case AudioEncoding::PcmS16LE:
    case AudioEncoding::PcmS24LE:
    case AudioEncoding::PcmS32LE:
    case AudioEncoding::PcmU8:
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case AudioEncoding::PcmF32LE:
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    default:
        return false;
    }
    return true;
}

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

    // 初始化全部成功：通知 start() 可以返回 true。
    started_.store(true, std::memory_order_release);

    const std::uint32_t frame_bytes = format_.frame_bytes();

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

        std::uint32_t frames_written = static_cast<std::uint32_t>(bytes_needed / frame_bytes);
        hr = render_client->ReleaseBuffer(frames_written, 0);
        if (FAILED(hr)) {
            log_warn_fmt("WASAPI playback: ReleaseBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
            break;
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
