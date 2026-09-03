#ifndef AQUA_AUDIO_DEVICES_AAUDIO_DEVICE_MANAGER_H
#define AQUA_AUDIO_DEVICES_AAUDIO_DEVICE_MANAGER_H

// Android 最小设备管理器。设计决议：aqua_core/doc/aaudio_backend_design.md §3；
// 播放设备路由扩展：playback_switching_design.md §8。
//
// Android 音频路由由 AudioPolicy 集中决策，AAudio 无设备枚举 API；
// 本实现提供「系统默认输出」合成条目，并放行 "android:N" 格式的显式
// 播放设备（N = Java 层 AudioManager 的 int device id，由 JNI 编码，
// playback backend 映射为 AAudioStreamBuilder_setDeviceId）。
// 设备切换 = PlaybackManager restart 事务（playback_switching_design.md）。
//
// capture 侧（后续阶段）：INPUT 方向同样跟随系统默认输入（麦克风）；
// default_format(INPUT) 待 capture backend 实现时按实际回读格式补充
// （设计决议 §4.1：实际格式如实上报为 server 契约）。

#include "aqua/audio/devices/audio_device_manager.h"

#include <cstdint>
#include <optional>

namespace aqua::audio::aaudio {

// 解析 "android:N" 为 AAudio device id（N 为 Java AudioManager int id）；
// 非该格式 / 数字非法返回 nullopt。backend 与 device manager 共用。
[[nodiscard]] std::optional<std::int32_t>
parse_aaudio_device_id(const AudioDeviceId& id) noexcept;

// 将 AAudio device id 编码回 "android:N"（stream_info 回读用）。
[[nodiscard]] std::string encode_aaudio_device_id(std::int32_t device_id);

class AAudioAudioDeviceManager final : public AudioDeviceManager {
public:
    AAudioAudioDeviceManager() = default;
    ~AAudioAudioDeviceManager() override = default;

    [[nodiscard]] std::vector<AudioDevice>
    enumerate(AudioDeviceDirection direction) const override;

    [[nodiscard]] std::optional<AudioDevice>
    default_device(AudioDeviceDirection direction) const override;

    // INPUT 方向暂不可用（capture backend 未实现）：返回 NotSupported。
    // OUTPUT 方向跟随 server 契约（client 侧格式来自 gRPC，不查询设备）：
    // 返回 InvalidArgument。
    [[nodiscard]] std::expected<AudioFormat, AudioError>
    default_format(AudioDeviceDirection direction,
        const std::optional<AudioDeviceId>& requested) const override;

    [[nodiscard]] std::expected<AudioDevice, AudioError>
    resolve(AudioDeviceDirection direction,
        const std::optional<AudioDeviceId>& requested) const override;
};

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_DEVICES_AAUDIO_DEVICE_MANAGER_H
