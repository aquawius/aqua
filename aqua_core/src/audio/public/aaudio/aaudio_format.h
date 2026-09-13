#ifndef AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H
#define AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H

// core AudioEncoding <-> aaudio_format_t mapping shared by the AAudio playback
// backend (and a future capture backend). Previously these lived in the playback
// backend's anonymous namespace; consolidated here for single-sourcing.

#include <aaudio/AAudio.h>

#include "aqua/audio/audio_format.h"

namespace aqua::audio::aaudio {

// core AudioEncoding -> aaudio_format_t。
// U8 无对应（AAudio 无 U8 格式）→ 返回 false，上层直接 FormatUnsupported。
[[nodiscard]] inline bool to_aaudio_format(AudioEncoding encoding, aaudio_format_t& out) noexcept
{
    switch (encoding) {
    case AudioEncoding::PCM_S16LE:
        out = AAUDIO_FORMAT_PCM_I16;
        return true;
    case AudioEncoding::PCM_S24LE:
        out = AAUDIO_FORMAT_PCM_I24_PACKED;
        return true;
    case AudioEncoding::PCM_S32LE:
        out = AAUDIO_FORMAT_PCM_I32;
        return true;
    case AudioEncoding::PCM_F32LE:
        out = AAUDIO_FORMAT_PCM_FLOAT;
        return true;
    case AudioEncoding::PCM_U8:
    case AudioEncoding::INVALID:
        return false;
    }
    return false;
}

// 回读的 aaudio_format_t -> core AudioEncoding。
[[nodiscard]] inline AudioEncoding from_aaudio_format(aaudio_format_t format) noexcept
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
        return AudioEncoding::INVALID;
    }
}

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_FORMAT_H
