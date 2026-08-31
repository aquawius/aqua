// Android 最小设备管理器实现。设计决议：aaudio_backend_design.md §3.2。

#include "audio/devices/aaudio/aaudio_device_manager.h"

#include "aqua/logger/logger.h"

namespace aqua::audio::aaudio {
namespace {

    // 合成的「跟随系统」设备条目：id 为空（不参与持久化/匹配），方向按请求。
    [[nodiscard]] AudioDevice make_system_default(AudioDeviceDirection direction)
    {
        AudioDevice device;
        device.id = AudioDeviceId { }; // 空 id = 系统默认
        device.name = direction == AudioDeviceDirection::INPUT
            ? "System Default Input"
            : "System Default Output";
        device.direction = direction;
        device.is_default = true;
        return device;
    }

} // namespace

std::vector<AudioDevice> AAudioAudioDeviceManager::enumerate(
    AudioDeviceDirection direction) const
{
    // Android 无设备枚举 API（设计决议 §3.1）；只报告系统默认条目，
    // 让 --list-devices 类 UI 有内容可显示而不是崩溃。
    return { make_system_default(direction) };
}

std::optional<AudioDevice> AAudioAudioDeviceManager::default_device(
    AudioDeviceDirection direction) const
{
    return make_system_default(direction);
}

std::expected<AudioFormat, AudioError> AAudioAudioDeviceManager::default_format(
    AudioDeviceDirection direction,
    const std::optional<AudioDeviceId>& requested) const
{
    (void)requested;
    if (direction == AudioDeviceDirection::INPUT) {
        // capture backend 未实现（设计决议 §4：后续阶段按实际回读格式上报）。
        return std::unexpected(AudioError::NotSupported);
    }
    // client playback 的格式来自 gRPC 契约，不从设备探测（设计决议 §3.2）。
    return std::unexpected(AudioError::InvalidArgument);
}

std::expected<AudioDevice, AudioError> AAudioAudioDeviceManager::resolve(
    AudioDeviceDirection direction,
    const std::optional<AudioDeviceId>& requested) const
{
    if (requested.has_value() && !requested->empty()) {
        // Android 显式设备选择不受支持（设计决议 §3.2）；
        // 唯一受支持路径是 Java 层 AudioManager id（后续 capture 阶段）。
        log_debug("AAudioAudioDeviceManager: explicit device selection is unsupported on Android");
        return std::unexpected(AudioError::DeviceNotFound);
    }
    if (direction == AudioDeviceDirection::INPUT) {
        // playback 阶段不解析 INPUT；capture backend 落地后开放。
        log_debug("AAudioAudioDeviceManager: INPUT resolution deferred to capture phase");
        return std::unexpected(AudioError::NotSupported);
    }
    return make_system_default(direction);
}

} // namespace aqua::audio::aaudio
