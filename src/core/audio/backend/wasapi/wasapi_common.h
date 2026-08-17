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

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <windows.h>

#include <cstring>
#include <optional>

namespace aqua::audio::wasapi {

// COM 指针 RAII 包装。避免在 capture/playback 两份代码中重复实现。
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p)
        : ptr_(p)
    {
    }
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept
        : ptr_(o.release())
    {
    }
    ComPtr& operator=(ComPtr&& o) noexcept
    {
        reset(o.release());
        return *this;
    }

    void reset(T* p = nullptr)
    {
        if (ptr_)
            ptr_->Release();
        ptr_ = p;
    }
    T* release()
    {
        T* t = ptr_;
        ptr_ = nullptr;
        return t;
    }
    T* get() const { return ptr_; }
    // 返回接收新指针的地址。先 reset() 释放旧指针，避免非空时泄漏旧 COM 引用。
    T** put()
    {
        reset();
        return &ptr_;
    }
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
inline std::optional<AudioFormat> wave_format_to_audio_format(const WAVEFORMATEX* wfx)
{
    if (!wfx)
        return std::nullopt;

    AudioFormat fmt;
    fmt.channels = wfx->nChannels;
    fmt.sample_rate = wfx->nSamplesPerSec;

    AudioEncoding encoding = AudioEncoding::Invalid;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            encoding = AudioEncoding::PcmF32LE;
        } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            // 24-in-32 等"有效位 < 容器位"的格式（wValidBitsPerSample != wBitsPerSample，
            // 有效数据左对齐在 32 位容器的高位）无法用现有 AudioEncoding 精确表示：
            // 按 S32LE(4B) 读会音量放大 256 倍、按 S24LE(3B) 读会字节错位。
            // 明确拒绝（返回 nullopt），让调用方报错而非静默产生坏音频。
            if (ext->Samples.wValidBitsPerSample != 0
                && ext->Samples.wValidBitsPerSample != wfx->wBitsPerSample) {
                return std::nullopt;
            }
            switch (wfx->wBitsPerSample) {
            case 8:
                encoding = AudioEncoding::PcmU8;
                break;
            case 16:
                encoding = AudioEncoding::PcmS16LE;
                break;
            case 24:
                encoding = AudioEncoding::PcmS24LE;
                break;
            case 32:
                encoding = AudioEncoding::PcmS32LE;
                break;
            }
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        switch (wfx->wBitsPerSample) {
        case 8:
            encoding = AudioEncoding::PcmU8;
            break;
        case 16:
            encoding = AudioEncoding::PcmS16LE;
            break;
        case 24:
            encoding = AudioEncoding::PcmS24LE;
            break;
        case 32:
            encoding = AudioEncoding::PcmS32LE;
            break;
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        encoding = AudioEncoding::PcmF32LE;
    }

    fmt.encoding = encoding;
    if (!fmt.valid())
        return std::nullopt;
    return fmt;
}

// 原生 AudioFormat -> WAVEFORMATEXTENSIBLE（用于 render 端设置共享模式格式）。
// 不支持的编码返回 false。
inline bool audio_format_to_wave_format(const AudioFormat& fmt, WAVEFORMATEXTENSIBLE& wfx)
{
    if (!fmt.valid())
        return false;
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
    case 1:
        wfx.dwChannelMask = SPEAKER_FRONT_CENTER;
        break;
    case 2:
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        break;
    default:
        wfx.dwChannelMask = 0;
        break; // 0 = unspecified, 让 WASAPI 自行决定
    }

    switch (fmt.encoding) {
    case AudioEncoding::PcmS16LE:
    case AudioEncoding::PcmS24LE:
        // 注意：S24LE 此处输出 packed 24-bit（wBitsPerSample=24，3 字节容器）。
        // WASAPI render 端通常期望 24-in-32（32 位容器 + 24 有效位），packed 24 可能
        // 被某些驱动拒绝。capture 端已拒绝 24-in-32，正常流程不会产生 S24LE 到 render；
        // 此为已知限制（需完整 24-in-32 支持时再处理，见 audio_format 编码扩展）。
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
