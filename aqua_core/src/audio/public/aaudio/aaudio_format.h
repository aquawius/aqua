#ifndef AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H
#define AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H

// core AudioEncoding <-> aaudio_format_t mapping shared by the AAudio playback
// backend (and a future capture backend). Previously these lived in the playback
// backend's anonymous namespace; consolidated here for single-sourcing.

#include <aaudio/AAudio.h>

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"

#include <expected>
#include <optional>

namespace aqua::audio::aaudio {

// core AudioEncoding -> aaudio_format_t。
// U8 无对应（AAudio 无 U8 格式）→ unexpected(FormatUnsupported)。
[[nodiscard]] inline std::expected<aaudio_format_t, AudioError> to_aaudio_format(
    AudioEncoding encoding) noexcept
{
    switch (encoding) {
    case AudioEncoding::PCM_S16LE:
        return AAUDIO_FORMAT_PCM_I16;
    case AudioEncoding::PCM_S24LE:
        return AAUDIO_FORMAT_PCM_I24_PACKED;
    case AudioEncoding::PCM_S32LE:
        return AAUDIO_FORMAT_PCM_I32;
    case AudioEncoding::PCM_F32LE:
        return AAUDIO_FORMAT_PCM_FLOAT;
    case AudioEncoding::PCM_U8:
    case AudioEncoding::INVALID:
        return std::unexpected(AudioError::FormatUnsupported);
    }
    return std::unexpected(AudioError::FormatUnsupported);
}

// 回读的 aaudio_format_t -> core AudioEncoding；未知格式返回 nullopt
// （取代 AudioEncoding::INVALID 哨兵：调用方必须显式处理"不认识"）。
[[nodiscard]] inline std::optional<AudioEncoding> from_aaudio_format(
    aaudio_format_t format) noexcept
{
    switch (format) {
    case AAUDIO_FORMAT_PCM_I16:
        return AudioEncoding::PCM_S16LE;
    case AAUDIO_FORMAT_PCM_I24_PACKED:
        return AudioEncoding::PCM_S24LE;
    case AAUDIO_FORMAT_PCM_I32:
        return AudioEncoding::PCM_S32LE;
    case AAUDIO_FORMAT_PCM_FLOAT:
        return AudioEncoding::PCM_F32LE;
    default:
        return std::nullopt;
    }
}

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H
