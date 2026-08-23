#ifndef AQUA_AUDIO_DEVICE_MANAGER_H
#define AQUA_AUDIO_DEVICE_MANAGER_H

// 设备系统入口：枚举 / 默认设备 / 按 ID 解析设备选择。
// requested == nullopt 表示“由当前平台选择默认设备”。
// 本类不持有音频流，不触碰 AudioCapture/AudioPlayback 的实时线程。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/devices/audio_device.h"

#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace aqua::audio {

class AudioDeviceManager {
public:
    virtual ~AudioDeviceManager() = default;

    [[nodiscard]] virtual std::vector<AudioDevice>
    enumerate(AudioDeviceDirection direction) const = 0;

    [[nodiscard]] virtual std::optional<AudioDevice>
    default_device(AudioDeviceDirection direction) const = 0;

    // 将“设备选择请求”解析成当前平台的具体设备。
    // requested == nullopt -> 平台默认设备；有值 -> 指定设备。
    // 返回的设备方向必须与 direction 一致。
    // 这是启动路径，因此用 expected 区分 DeviceNotFound / BackendFailed 等失败原因。
    [[nodiscard]] virtual std::expected<AudioDevice, AudioError>
    resolve(AudioDeviceDirection direction,
        const std::optional<AudioDeviceId>& requested) const = 0;
};

// 工厂：创建当前平台的设备管理器（如 WASAPI）。该平台尚未实现时返回 nullptr。
std::unique_ptr<AudioDeviceManager> create_device_manager();

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEVICE_MANAGER_H
