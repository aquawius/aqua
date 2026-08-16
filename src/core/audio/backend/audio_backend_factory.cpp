#include "core/audio/backend/audio_backend_factory.h"

#if defined(_WIN32)
#include "core/audio/backend/wasapi/wasapi_capture.h"
#include "core/audio/backend/wasapi/wasapi_playback.h"
#elif defined(__ANDROID__)
#include "core/audio/backend/aaudio/aaudio_playback.h"
#endif

namespace aqua::audio {

std::unique_ptr<CaptureBackend> create_capture_backend()
{
#if defined(_WIN32)
    return std::make_unique<WasapiCapture>();
#else
    // Android mic 采集（AAudio input）尚未实现，后续里程碑补充。
    return nullptr;
#endif
}

std::unique_ptr<PlaybackBackend> create_playback_backend()
{
#if defined(_WIN32)
    return std::make_unique<WasapiPlayback>();
#elif defined(__ANDROID__)
    return std::make_unique<AaudioPlayback>();
#else
    return nullptr;
#endif
}

} // namespace aqua::audio
