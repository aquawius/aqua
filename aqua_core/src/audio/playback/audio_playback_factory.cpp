#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/playback/audio_playback.h"

#ifdef _WIN32
#endif

namespace aqua::audio {

std::unique_ptr<AudioPlayback> create_playback(AudioDeviceManager& device_manager)
{
#ifdef _WIN32
    return nullptr;
#else
    static_cast<void>(device_manager);
    // Linux / Android 后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
