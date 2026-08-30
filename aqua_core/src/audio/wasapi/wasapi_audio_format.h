#ifndef AQUA_WASAPI_AUDIO_FORMAT_H
#define AQUA_WASAPI_AUDIO_FORMAT_H

// Internal WASAPI format conversion shared by device probing and stream startup.
// Keeping this mapping in one place prevents the backend "default_format()" probe
// from drifting away from the actual capture/playback format interpretation.

#include <aqua/audio/audio_format.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <windows.h>

#include <cstdint>
#include <optional>

namespace aqua::audio::wasapi {

[[nodiscard]] inline bool guid_equal(const GUID& lhs, const GUID& rhs) noexcept
{
    return ::IsEqualGUID(lhs, rhs) != FALSE;
}

[[nodiscard]] inline std::optional<AudioEncoding> audio_encoding_from_wave_format(
    const WAVEFORMATEX& format, std::uint16_t& container_bits) noexcept
{
    container_bits = format.wBitsPerSample;

    GUID subformat = GUID_NULL;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return std::nullopt;
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        subformat = extensible.SubFormat;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        subformat = KSDATAFORMAT_SUBTYPE_PCM;
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subformat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else {
        return std::nullopt;
    }

    if (guid_equal(subformat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && container_bits == 32) {
        return AudioEncoding::PCM_F32LE;
    }
    if (!guid_equal(subformat, KSDATAFORMAT_SUBTYPE_PCM)) {
        return std::nullopt;
    }

    switch (container_bits) {
    case 8:
        return AudioEncoding::PCM_U8;
    case 16:
        return AudioEncoding::PCM_S16LE;
    case 24:
        return AudioEncoding::PCM_S24LE;
    case 32:
        return AudioEncoding::PCM_S32LE;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<AudioFormat> audio_format_from_wave_format(
    const WAVEFORMATEX& format) noexcept
{
    if (format.nChannels == 0 || format.nSamplesPerSec == 0) {
        return std::nullopt;
    }

    std::uint16_t container_bits = 0;
    const auto encoding = audio_encoding_from_wave_format(format, container_bits);
    if (!encoding) {
        return std::nullopt;
    }

    AudioFormat result {
        .encoding = *encoding,
        .channels = format.nChannels,
        .sample_rate = format.nSamplesPerSec,
    };

    if (!result.is_valid() || format.nBlockAlign != result.frame_bytes()) {
        return std::nullopt;
    }

    return result;
}

} // namespace aqua::audio::wasapi

#endif // AQUA_WASAPI_AUDIO_FORMAT_H
