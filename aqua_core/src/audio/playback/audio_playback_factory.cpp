#include "aqua/audio/playback/audio_playback.h"

#include "aqua/audio/devices/audio_device_manager.h"

#ifdef _WIN32
#include "audio/playback/wasapi/wasapi_audio_playback.h"
#endif

namespace aqua::audio {

std::unique_ptr<AudioPlayback> create_playback(AudioDeviceManager& device_manager)
{
#ifdef _WIN32
    return std::make_unique<wasapi::WasapiAudioPlayback>(device_manager);
#else
    static_cast<void>(device_manager);
    // Linux / Android / macOS 后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
