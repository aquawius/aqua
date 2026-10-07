#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ENGINE_PERIOD_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ENGINE_PERIOD_H

// WASAPI 共享模式"引擎周期"折算，shared by the playback and capture backends.
// 两条链路此前各自只会"要最小周期"或"完全不请求周期"，把同一份算术放在这里
// 是为了：① 回放/采集取到同一个语义；② 这段四边界折算有取整与钳制，
// 单测覆盖它比覆盖两条完整流便宜得多（无需音频设备）。
//
// 本文件**有意不依赖 Windows SDK**：输入就是
// `IAudioClient3::GetSharedModeEnginePeriod` 直接给出的四个边界值
//（在 Windows 上 `UINT32` 即 `unsigned int`，与 `std::uint32_t` 同类型）。

#include <algorithm>
#include <cstdint>

namespace aqua::audio::wasapi {

// 把"请求的缓冲帧数"折算成音频引擎可接受的周期。引擎契约：周期必须是
// fundamental 的整数倍，且落在 [minimum, maximum] 内
// （IAudioClient3::GetSharedModeEnginePeriod 给出的四条边界）。
//
// requested == 0 的语义是"**设备允许的最小周期**"，即本选项能达到的最低
// 延迟——而不是引擎默认周期。默认周期通常就是 shared 引擎的 10ms 节拍
// （480 帧 @48k），把它当默认等于放弃了 IAudioClient3 存在的意义。
// 想退回"引擎默认"请显式传一个等于 default 的帧数（日志会打印它）。
//
// ⚠️ 这里只决定**音频设备 I/O 粒度**，与链路层每包帧数 F 无关：F 由 MTU
// 预算（1400B payload）与 5ms 时长上限决定，是**网络分包**粒度。两者唯一
// 交汇点是几何地板 ceil(设备回调帧数 / F) + 1 —— "一次回调用掉几个网络
// 包"，它同时是 JB target 的硬下限（ADR-3）。把设备周期调小会让这个地板
// 一起变小，但不会改变每槽的毫秒数（那由 F 决定）。
//
// 返回 0 = 这台设备/格式拿不到合法周期，调用方应退回"不请求周期"的路径
// （那是引擎默认周期，见 capture 后端的说明）。
[[nodiscard]] inline std::uint32_t choose_engine_period(
    std::uint32_t requested,
    std::uint32_t fundamental,
    std::uint32_t minimum,
    std::uint32_t maximum) noexcept
{
    if (fundamental == 0 || minimum == 0 || maximum < minimum) {
        return 0;
    }

    const std::uint32_t min_multiple = (minimum + fundamental - 1U) / fundamental;
    const std::uint32_t max_multiple = maximum / fundamental;
    if (min_multiple > max_multiple) {
        return 0;
    }

    if (requested == 0) {
        // 最小可选周期 = 向上取整到 fundamental 的首个倍数，必在 max 之内
        // （min_multiple <= max_multiple 已在上方保证）。
        return min_multiple * fundamental;
    }

    std::uint64_t requested_multiple = (static_cast<std::uint64_t>(requested) + fundamental / 2U) / fundamental;
    // 三个实参显式同类型：std::clamp<T> 的 lo/hi 是 const T&，传 uint32 会各绑一个临时量，
    // 结果虽然在本表达式内有效（此处立刻取值），但没必要依赖这条。
    requested_multiple = std::clamp<std::uint64_t>(requested_multiple,
        static_cast<std::uint64_t>(min_multiple), static_cast<std::uint64_t>(max_multiple));

    return static_cast<std::uint32_t>(requested_multiple * fundamental);
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ENGINE_PERIOD_H
