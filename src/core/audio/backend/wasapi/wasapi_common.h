#ifndef AQUA_WASAPI_COMMON_H
#define AQUA_WASAPI_COMMON_H

// Windows WASAPI 后端公共工具：ComPtr 与 WAVEFORMATEX 互转。
// 仅在 _WIN32 下使用，由 wasapi_capture.cpp / wasapi_playback.cpp 内部包含。

#include "core/public/audio_format.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>

#include <optional>
#include <cstring>

namespace aqua::audio::wasapi {

// COM 指针 RAII 包装。避免在 capture/playback 两份代码中重复实现。
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

// RAII: 提高 Windows 定时器分辨率到 1ms，使 sleep_for(1ms) 实际生效。
// 不调用时 Windows 默认粒度 ~15.6ms，导致音频线程轮询延迟。
class WindowsTimerResolution {
public:
    WindowsTimerResolution() { timeBeginPeriod(1); }
    ~WindowsTimerResolution() { timeEndPeriod(1); }
    WindowsTimerResolution(const WindowsTimerResolution&) = delete;
    WindowsTimerResolution& operator=(const WindowsTimerResolution&) = delete;
};

// 设备 mix format (WAVEFORMATEX) -> 原生 AudioFormat。
// 不支持的编码返回 std::nullopt。
inline std::optional<AudioFormat> wave_format_to_audio_format(const WAVEFORMATEX* wfx) {
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
            case 8:  encoding = AudioEncoding::PcmU8;    break;
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

// 原生 AudioFormat -> WAVEFORMATEXTENSIBLE（用于 render 端设置共享模式格式）。
// 不支持的编码返回 false。
inline bool audio_format_to_wave_format(const AudioFormat& fmt, WAVEFORMATEXTENSIBLE& wfx) {
    if (!fmt.valid()) return false;
    std::memset(&wfx, 0, sizeof(wfx));
    wfx.Format.nChannels = static_cast<WORD>(fmt.channels);
    wfx.Format.nSamplesPerSec = fmt.sample_rate;
    wfx.Format.wBitsPerSample = static_cast<WORD>(fmt.bytes_per_sample() * 8);
    wfx.Format.nBlockAlign = static_cast<WORD>(fmt.frame_bytes());
    wfx.Format.nAvgBytesPerSec = fmt.sample_rate * fmt.frame_bytes();
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
    // 根据声道数推导通道掩码，避免硬编码立体声导致单声道/多声道格式被拒绝
    switch (fmt.channels) {
    case 1:  wfx.dwChannelMask = SPEAKER_FRONT_CENTER; break;
    case 2:  wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; break;
    default: wfx.dwChannelMask = 0; break;  // 0 = unspecified, 让 WASAPI 自行决定
    }

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

} // namespace aqua::audio::wasapi

#endif // AQUA_WASAPI_COMMON_H
