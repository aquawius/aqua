#ifndef AQUA_AUDIO_DEVICES_DEVICE_SET_POLLER_H
#define AQUA_AUDIO_DEVICES_DEVICE_SET_POLLER_H

// 设备集合轮询 + 去抖：把「每 control tick 枚举一次」收敛成「确认过的变化才上报」。
//
// 两条约束都来自实现事实，不是假想需求：
//
// 1. 单次采样不可信。WASAPI 在设备转换期可能瞬时枚举不全；一次采样就交给路由
//    决策，会把「活跃设备缺席」误判成设备被拔，从而 restart 一条健康的流。
//    误判三次就烧光 10s/3 的重试预算落 Fatal——比不轮询更糟。因此变化必须
//    连续 kConfirmations 次采样一致才上报（500ms tick 下约 0.5~1s 确认延迟，
//    与 notify 路径的 1s 合并窗口同量级）。
//
// 2. 有些平台没有可枚举的设备集合。AAudioAudioDeviceManager::enumerate 只返回
//    一条**空 id** 的合成条目（Android 无设备枚举 API，真实列表由 Kotlin
//    AudioManager 经 aqua_client_notify_devices_changed 推送）。把它当真相等于
//    每个 tick 都判定活跃设备缺席。因此「采样中不含任何非空 id」一律按**无信息**
//    处理：返回 nullopt，不去抖、不动基线——调用方不得动作，设备集合改由上层
//    推送驱动（DeviceSetPoller 与 notify 路径共用 manager 的同一套决策，
//    互不干扰）。
//
// 无信息的失败模式是惰性的（什么都不做），这比误判安全：设备真被拔掉时流会死，
// backend 的流错误事件仍会驱动恢复。轮询是**提前**发现，不是唯一发现途径。

#include "aqua/audio/devices/audio_device.h"
#include "aqua/audio/devices/audio_device_manager.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aqua::audio {

// 一次已确认的设备集合变化。
struct DeviceSetChange {
    // 当前集合（按 id 有序；枚举顺序不保证稳定，不作变化判据）。
    std::vector<AudioDeviceId> present;
    // 相对上一次**上报**的集合：新插入 / 已拔出。
    // 首次上报是基线，两者皆空（初始设备列表不是"新增设备"）。
    std::vector<AudioDeviceId> added;
    std::vector<AudioDeviceId> removed;
};

// 诊断用：把 id 集合拼成 "[a, b]"；空集合为 "[]"。
[[nodiscard]] std::string format_device_ids(const std::vector<AudioDeviceId>& ids);

class DeviceSetPoller {
public:
    DeviceSetPoller(AudioDeviceManager& devices, AudioDeviceDirection direction) noexcept
        : devices_(&devices)
        , direction_(direction)
    {
    }

    // 每个 control tick（RUNTIME_CONTROL_POLL_INTERVAL，500ms）调用一次。
    // 返回 nullopt = 本次无可上报的变化（平台不可枚举 / 去抖未通过 / 与上次
    // 上报一致）。只在控制线程调用（与 manager 的 tick() 同线程串行，故无锁）。
    [[nodiscard]] std::optional<DeviceSetChange> poll() noexcept;

    // 丢弃采样、去抖与基线状态（新会话开始；对称 manager 的 known_devices_valid_）。
    void reset() noexcept;

private:
    // 连续一致采样门槛：2 次 = 一个 tick 周期的确认延迟。
    static constexpr unsigned kConfirmations = 2;

    // 枚举并规范化成有序 id 集合；剔除空 id（合成的"跟随系统"条目）。
    // nullopt = 无信息（抛异常 / 原始列表含空 id 项而过滤后为空，即 Android
    // 合成条目）。原始空列表（WASAPI 真零设备）是有效空集，不是无信息——
    // 全拔场景下 active 必消失，须能上报，否则 capture“主动 Fatal”保证失效。
    [[nodiscard]] std::optional<std::vector<AudioDeviceId>> enumerate_ids() const noexcept;

    AudioDeviceManager* devices_;
    AudioDeviceDirection direction_;
    // 去抖中：最近一次采样与其连续命中次数。
    std::vector<AudioDeviceId> sampled_;
    unsigned sample_hits_ = 0;
    // 上一次**上报**的集合：变化判定与 added/removed 的基准。
    std::vector<AudioDeviceId> reported_;
    bool has_reported_ = false;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEVICES_DEVICE_SET_POLLER_H
