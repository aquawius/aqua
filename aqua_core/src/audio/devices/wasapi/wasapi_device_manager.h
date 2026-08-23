#ifndef AQUA_AUDIO_WASAPI_DEVICE_MANAGER_H
#define AQUA_AUDIO_WASAPI_DEVICE_MANAGER_H

#include "aqua/audio/devices/audio_device_manager.h"

namespace aqua::audio::wasapi {

class WasapiAudioDeviceManager final : public AudioDeviceManager {
public:
    WasapiAudioDeviceManager() = default;
    ~WasapiAudioDeviceManager() override = default;

    [[nodiscard]] std::vector<AudioDevice>
    enumerate(AudioDeviceDirection direction) const override;

    [[nodiscard]] std::optional<AudioDevice>
    default_device(AudioDeviceDirection direction) const override;

    [[nodiscard]] std::expected<AudioDevice, AudioError>
    resolve(AudioDeviceDirection direction,
        const std::optional<AudioDeviceId>& requested) const override;
};

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_WASAPI_DEVICE_MANAGER_H
