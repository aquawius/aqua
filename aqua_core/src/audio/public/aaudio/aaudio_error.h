#ifndef AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_ERROR_H
#define AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_ERROR_H

// aaudio_result_t -> AudioError mapping and result-name lookup shared by the
// AAudio playback backend (and a future capture backend). Previously these lived
// in the playback backend's anonymous namespace; consolidated here.

#include <aaudio/AAudio.h>

#include "aqua/audio/audio_error.h"

#include <string>

namespace aqua::audio::aaudio {

[[nodiscard]] inline AudioError map_aaudio_error(aaudio_result_t result) noexcept
{
    switch (result) {
    case AAUDIO_ERROR_INVALID_FORMAT:
        return AudioError::FormatUnsupported;
    case AAUDIO_ERROR_DISCONNECTED:
        return AudioError::DeviceDisconnected;
    case AAUDIO_ERROR_INTERNAL:
    case AAUDIO_ERROR_UNAVAILABLE:
    case AAUDIO_ERROR_NO_FREE_HANDLES:
    case AAUDIO_ERROR_NO_MEMORY:
    case AAUDIO_ERROR_TIMEOUT:
        return AudioError::BackendFailed;
    default:
        return AudioError::BackendFailed;
    }
}

[[nodiscard]] inline std::string aaudio_result_name(aaudio_result_t result)
{
    const char* name = AAudio_convertResultToText(result);
    return name != nullptr ? std::string(name) : std::string("unknown");
}

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PUBLIC_AAUDIO_AAUDIO_ERROR_H
