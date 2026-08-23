#include "aqua/audio/capture/audio_capture.h"

#ifdef _WIN32
#endif

namespace aqua::audio {

std::unique_ptr<AudioCapture> create_capture(AudioDeviceManager& device_manager)
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
