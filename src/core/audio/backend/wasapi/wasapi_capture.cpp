#include "core/audio/backend/wasapi/wasapi_capture.h"

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
#include <thread>

namespace aqua::audio {

namespace {

// COM RAII
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : ptr_(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& o) noexcept : ptr_(o.release()) {}
    ComPtr& operator=(ComPtr&& o) noexcept {
        reset(o.release());
        return *this;
    }

    void reset(T* p = nullptr) {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* release() { T* t = ptr_; ptr_ = nullptr; return t; }
    T* get() const { return ptr_; }
    T** put() { return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// 将 WAVEFORMATEX 转换为 AudioFormat
std::optional<AudioFormat> wave_format_to_audio_format(const WAVEFORMATEX* wfx) {
    if (!wfx) return std::nullopt;

    AudioFormat fmt;
    fmt.channels = wfx->nChannels;
    fmt.sample_rate = wfx->nSamplesPerSec;

    AudioEncoding encoding = AudioEncoding::Invalid;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            encoding = AudioEncoding::PcmF32LE;
        } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            switch (wfx->wBitsPerSample) {
            case 16: encoding = AudioEncoding::PcmS16LE; break;
            case 24: encoding = AudioEncoding::PcmS24LE; break;
            case 32: encoding = AudioEncoding::PcmS32LE; break;
            }
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        switch (wfx->wBitsPerSample) {
        case 8:  encoding = AudioEncoding::PcmU8;    break;
        case 16: encoding = AudioEncoding::PcmS16LE; break;
        case 24: encoding = AudioEncoding::PcmS24LE; break;
        case 32: encoding = AudioEncoding::PcmS32LE; break;
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        encoding = AudioEncoding::PcmF32LE;
    }

    fmt.encoding = encoding;
    if (!fmt.valid()) return std::nullopt;
    return fmt;
}

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
    thread_ = std::thread(&WasapiCapture::capture_loop, this);

    // 等待线程初始化完成并设置 format
    for (int i = 0; i < 100 && format_.encoding == AudioEncoding::Invalid; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (format_.encoding == AudioEncoding::Invalid) {
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
}

bool WasapiCapture::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

void WasapiCapture::capture_loop()
{
    // COM 初始化（STA 也可，这里用 MTA）
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

    // 获取 mix format
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        log_error_fmt("WASAPI capture: GetMixFormat failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
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

    ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(IID_PPV_ARGS(capture_client.put()));
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: GetService capture failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        log_error_fmt("WASAPI capture: Start failed: 0x{:08X}", static_cast<unsigned>(hr));
        if (com_initialized) CoUninitialize();
        running_ = false;
        return;
    }

    log_info_fmt("WASAPI capture started: {}ch {}Hz encoding={}",
                 format_.channels, format_.sample_rate, static_cast<int>(format_.encoding));

    const std::uint32_t frame_bytes = format_.frame_bytes();

    // 采集循环
    while (running_) {
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

        if (num_frames > 0 && data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            const std::size_t byte_size = static_cast<std::size_t>(num_frames) * frame_bytes;
            if (callback_) {
                callback_(std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(data), byte_size});
            }
        }

        hr = capture_client->ReleaseBuffer(num_frames);
        if (FAILED(hr)) {
            log_warn_fmt("WASAPI capture: ReleaseBuffer failed: 0x{:08X}", static_cast<unsigned>(hr));
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
