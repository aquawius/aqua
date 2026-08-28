#include "aqua/audio/capture/audio_capture.h"

#ifdef _WIN32
#include "audio/capture/wasapi/wasapi_audio_capture.h"
#endif

namespace aqua::audio {

std::unique_ptr<AudioCapture> create_capture(AudioDeviceManager& device_manager)
{
#ifdef _WIN32
    return std::make_unique<wasapi::WasapiAudioCapture>(device_manager);
#else
    static_cast<void>(device_manager);
    // Linux / macOS / Android 后端尚未实现。
    return nullptr;
#endif
}

} // namespace aqua::audio
