#ifndef AQUA_AUDIO_DEVICE_H
#define AQUA_AUDIO_DEVICE_H

// 音频设备描述：值语义，不持有任何平台对象或流对象。

#include <compare>
#include <string>
#include <utility>

namespace aqua::audio {

enum class AudioDeviceDirection {
    NONE,   // 仅作“未初始化”哨兵；枚举/解析结果不会返回该值。
    INPUT,  // 输入 endpoint（麦克风等）。
    OUTPUT, // 输出 endpoint（扬声器/耳机等）。
};

// 不透明设备标识。约定：
//   - 同一平台会话内对同一设备稳定（可用于相等比较与缓存）；
//   - 跨会话、跨平台、跨机器无稳定性保证，也不可比较；
//   - 具体格式由 backend 决定（Windows endpoint id / Android device id 等）。
class AudioDeviceId {
public:
    AudioDeviceId() = default;

    explicit AudioDeviceId(std::string value)
        : value_(std::move(value))
    {
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

    auto operator<=>(const AudioDeviceId&) const = default;

private:
    std::string value_;
};

struct AudioDevice {
    AudioDeviceId id;
    std::string name;
    AudioDeviceDirection direction = AudioDeviceDirection::NONE;

    // 是否为该方向的系统默认设备。
    bool is_default = false;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEVICE_H
