// Android 最小设备管理器实现。设计决议：aaudio_backend_design.md §3.2；
// 播放设备路由扩展：playback_switching_design.md §8。

#include "audio/devices/aaudio/aaudio_device_manager.h"

#include "aqua/logger/logger.h"

#include <charconv>
#include <cstdint>
#include <string_view>

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

    // "android:N" 编码（playback_switching_design.md §9）：N = Java 层
    // AudioManager 的 int device id，JNI 侧直接编码，Kotlin 无字符串拼接。
    constexpr std::string_view kAndroidDevicePrefix = "android:";

    // 解析 "android:N" 为 AAudio device id；非该格式返回 nullopt。
    [[nodiscard]] std::optional<std::int32_t> parse_android_device_id(
        const AudioDeviceId& id) noexcept
    {
        const std::string_view value = id.value();
        if (!value.starts_with(kAndroidDevicePrefix)) {
            return std::nullopt;
        }
        const auto number = value.substr(kAndroidDevicePrefix.size());
        std::int32_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(
            number.data(), number.data() + number.size(), parsed);
        if (ec != std::errc {} || ptr != number.data() + number.size()) {
            return std::nullopt;
        }
        return parsed;
    }

} // namespace

std::vector<AudioDevice> AAudioAudioDeviceManager::enumerate(
    AudioDeviceDirection direction) const
{
    // Android 无设备枚举 API（设计决议 §3.1）；设备列表由 Kotlin 层
    // AudioManager.getDevices() 提供，native 只报告系统默认条目，
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
    if (direction == AudioDeviceDirection::INPUT) {
        // playback 阶段不解析 INPUT；capture backend 落地后开放。
        log_debug("AAudioAudioDeviceManager: INPUT resolution deferred to capture phase");
        return std::unexpected(AudioError::NotSupported);
    }
    if (requested.has_value() && !requested->empty()) {
        // 播放设备路由（playback_switching_design.md §8）：放行
        // "android:N"（Java 层 AudioManager id）；其余格式不支持。
        if (parse_android_device_id(*requested).has_value()) {
            AudioDevice device;
            device.id = *requested;
            device.name = "Android device " + std::string(requested->value());
            device.direction = direction;
            device.is_default = false;
            return device;
        }
        log_debug_fmt(
            "AAudioAudioDeviceManager: unsupported device id format '{}'",
            requested->value());
        return std::unexpected(AudioError::DeviceNotFound);
    }
    return make_system_default(direction);
}

// 头文件声明的共享解析/编码（backend 与 device manager 共用）。
std::optional<std::int32_t> parse_aaudio_device_id(const AudioDeviceId& id) noexcept
{
    return parse_android_device_id(id);
}

std::string encode_aaudio_device_id(std::int32_t device_id)
{
    return "android:" + std::to_string(device_id);
}

} // namespace aqua::audio::aaudio
