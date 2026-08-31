#include "aqua/audio/devices/audio_device_manager.h"

#include "aqua/logger/logger.h"

#ifdef _WIN32
#include "audio/devices/wasapi/wasapi_device_manager.h"
#elif defined(__ANDROID__)
#include "audio/devices/aaudio/aaudio_device_manager.h"
#endif

namespace aqua::audio {

std::unique_ptr<AudioDeviceManager> create_device_manager()
{
#ifdef _WIN32
    log_debug("AudioDeviceManager factory: selecting WASAPI backend");
    return std::make_unique<wasapi::WasapiAudioDeviceManager>();
#elif defined(__ANDROID__)
    log_debug("AudioDeviceManager factory: selecting AAudio backend");
    return std::make_unique<aaudio::AAudioAudioDeviceManager>();
#else
    log_debug("AudioDeviceManager factory: no backend available on this platform");
    // Linux（PipeWire/ALSA）与 macOS（CoreAudio）后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
