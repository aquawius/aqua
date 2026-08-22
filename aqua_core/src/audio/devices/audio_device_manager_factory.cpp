#include "aqua/audio/devices/audio_device_manager.h"

#ifdef _WIN32
#include "audio/devices/wasapi/wasapi_device_manager.h"
#endif

namespace aqua::audio {

std::unique_ptr<AudioDeviceManager> create_device_manager()
{
#ifdef _WIN32
    return std::make_unique<wasapi::WasapiAudioDeviceManager>();
#else
    // Linux（PipeWire/ALSA）与 Android（AAudio）后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
