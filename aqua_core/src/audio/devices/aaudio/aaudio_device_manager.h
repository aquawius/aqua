#ifndef AQUA_AUDIO_DEVICES_AAUDIO_DEVICE_MANAGER_H
#define AQUA_AUDIO_DEVICES_AAUDIO_DEVICE_MANAGER_H

// Android 最小设备管理器。设计决议：aqua_core/doc/aaudio_backend_design.md §3。
//
// Android 音频路由由 AudioPolicy 集中决策，AAudio 无设备枚举 API；
// 本实现只提供「系统默认输出」合成条目，显式 device id 一律 DeviceNotFound。
// 设备切换 = stop -> start（runtime 一次性生命周期），流断走
// DeviceDisconnected -> Degraded 既有链路。
//
// capture 侧（后续阶段）：INPUT 方向同样跟随系统默认输入（麦克风）；
// default_format(INPUT) 待 capture backend 实现时按实际回读格式补充
// （设计决议 §4.1：实际格式如实上报为 server 契约）。

#include "aqua/audio/devices/audio_device_manager.h"

namespace aqua::audio::aaudio {

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
