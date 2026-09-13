#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_FORMAT_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_FORMAT_H

// core AudioFormat -> WASAPI WAVEFORMATEX / WAVEFORMATEXTENSIBLE conversion,
// shared by the playback, capture and device-manager backends. Previously each
// of those translation units carried its own copy of this mapping; consolidating
// it here keeps the behaviour single-sourced.

// clang-format off: windows.h 必须先于 audioclient.h/ksmedia.h/mmreg.h 等依赖
// 它的 SDK 头，否则基础类型未定义；顺序 load-bearing，禁止排序。
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmreg.h>
// clang-format on

#include "aqua/audio/audio_format.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace aqua::audio::wasapi {

// Holds either a plain WAVEFORMATEX or a WAVEFORMATEXTENSIBLE (when channel
// count exceeds 2). `get()` returns the active layout.
struct WaveFormatStorage {
    WAVEFORMATEX basic { };
    WAVEFORMATEXTENSIBLE extensible { };
    bool is_extensible = false;

    [[nodiscard]] WAVEFORMATEX* get() noexcept
    {
        return is_extensible ? &extensible.Format : &basic;
    }
};

[[nodiscard]] inline std::optional<WaveFormatStorage>
make_wave_format(const AudioFormat& format) noexcept
{
    if (!format.is_valid() || format.channels > static_cast<std::uint32_t>(std::numeric_limits<WORD>::max())) {
        return std::nullopt;
    }

    const std::uint32_t frame_bytes = format.frame_bytes();
    const std::uint32_t bits = format.bytes_per_sample() * 8U;
    if (frame_bytes == 0 || bits > static_cast<std::uint32_t>(std::numeric_limits<WORD>::max())) {
        return std::nullopt;
    }

    WaveFormatStorage result;
    const bool use_extensible = format.channels > 2;

    if (!use_extensible) {
        auto& wave = result.basic;
        wave.nChannels = static_cast<WORD>(format.channels);
        wave.nSamplesPerSec = format.sample_rate;
        wave.wBitsPerSample = static_cast<WORD>(bits);
        wave.nBlockAlign = static_cast<WORD>(frame_bytes);
        wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
        wave.cbSize = 0;

        switch (format.encoding) {
        case AudioEncoding::PCM_F32LE:
            wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            break;
        case AudioEncoding::PCM_U8:
        case AudioEncoding::PCM_S16LE:
        case AudioEncoding::PCM_S24LE:
        case AudioEncoding::PCM_S32LE:
            wave.wFormatTag = WAVE_FORMAT_PCM;
            break;
        case AudioEncoding::INVALID:
            return std::nullopt;
        }
        return result;
    }

    auto& wave = result.extensible;
    result.is_extensible = true;
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(format.channels);
    wave.Format.nSamplesPerSec = format.sample_rate;
    wave.Format.wBitsPerSample = static_cast<WORD>(bits);
    wave.Format.nBlockAlign = static_cast<WORD>(frame_bytes);
    wave.Format.nAvgBytesPerSec = wave.Format.nSamplesPerSec * wave.Format.nBlockAlign;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.dwChannelMask = 0;
    wave.Samples.wValidBitsPerSample = wave.Format.wBitsPerSample;

    switch (format.encoding) {
    case AudioEncoding::PCM_F32LE:
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case AudioEncoding::PCM_U8:
    case AudioEncoding::PCM_S16LE:
    case AudioEncoding::PCM_S24LE:
    case AudioEncoding::PCM_S32LE:
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case AudioEncoding::INVALID:
        return std::nullopt;
    }

    return result;
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_FORMAT_H
