#include "aqua/audio/playback/audio_playback.h"

#include "aqua/logger/logger.h"

#include "aqua/audio/devices/audio_device_manager.h"

#ifdef _WIN32
#include "audio/playback/wasapi/wasapi_audio_playback.h"
#elif defined(__ANDROID__)
#include "audio/playback/aaudio/aaudio_audio_playback.h"
#endif

namespace aqua::audio {

std::unique_ptr<AudioPlayback> create_playback(AudioDeviceManager& device_manager)
{
#ifdef _WIN32
    log_debug("AudioPlayback factory: selecting WASAPI backend");
    return std::make_unique<wasapi::WasapiAudioPlayback>(device_manager);
#elif defined(__ANDROID__)
    log_debug("AudioPlayback factory: selecting AAudio backend");
    return std::make_unique<aaudio::AAudioAudioPlayback>(device_manager);
#else
    static_cast<void>(device_manager);
    log_debug("AudioPlayback factory: no backend available on this platform");
    // Linux / macOS 后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
