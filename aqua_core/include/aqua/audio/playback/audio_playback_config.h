#ifndef AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H
#define AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H

// 回放配置（值语义）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_route.h"

#include <cstdint>
#include <limits>

namespace aqua::audio {

struct AudioPlaybackConfig {
    // 路由请求（唯一的设备选择入口）：System = 由平台选择并后续变更端点，
    // Application = 把这条流绑定到 route.device。
    //
    // 注意这是「请求」而非「策略」：绑定失败时是降级还是 Fatal 由 manager 的
    // RoutePolicy 决定（见 devices/route_policy.h），client 侧恒为降级。
    AudioRoute route = AudioRoute::follow_system();

    // 回放格式：回调按该格式填充 output。设备不支持时 start() 返回 FormatUnsupported；
    // 回放不做转换，转换（如需）由上层在喂给回调之前完成。
    AudioFormat format;

    // 请求的缓冲大小（帧；所有声道合计的一组 sample frame）。
    //
    // **逐后端语义不同**，各自按自己的契约折算，实际回调粒度可以不同：
    //   - WASAPI：0（默认）= 向 IAudioClient3 要 `GetSharedModeEnginePeriod` 的
    //     minimum_period（向上取到 fundamental 倍数），即**设备允许的最小周期**，
    //     也就是本选项能达到的最低延迟；非 0 = 折算到最接近的 fundamental 倍数并钳到
    //     [min_period, max_period]。引擎默认周期（shared 引擎 10ms ≈ 480 帧 @48k）
    //     **不再**是默认值，要退回它得显式传该帧数。
    //   - AAudio：0（默认）= **不设置容量**，由 AAudio 按 performance mode 自行选择
    //     缓冲（Android 侧的低延迟主要来自 `low_latency` = PERFORMANCE_MODE_LOW_LATENCY，
    //     默认开）；非 0 = 只设**容量上限**（`setBufferCapacityInFrames`），实际 buffer
    //     size 由 AAudio 决定且可小于容量 —— 真正决定 AAudio 延迟的是 buffer *size*
    //     （`AAudioStream_setBufferSizeInFrames`），本后端不设置它。
    //
    // ⚠️ 这是**音频设备 I/O 粒度**，与链路层每包帧数 F 不是一回事：F 由 MTU
    // 预算（1400B payload）与 5ms 时长上限决定，属于**网络分包**粒度。两者唯一
    // 交汇点是几何地板 ceil(本值 / F) + 1（= "一次回调用掉几个网络包"），而它
    // 同时是自适应 target 的硬下限（ADR-3）——这就是低延迟场景要让本值变小的
    // 原因。调小它换到的是更紧的实时调度裕度：CPU 跟不上时的丢帧发生在设备层，
    // jitter buffer 观察不到。
    std::uint32_t frames_per_buffer = 0;

    // 是否请求低延迟播放路径。Android/AAudio 使用此选项选择
    // LOW_LATENCY + SHARED 或 NONE + SHARED；其它后端应忽略。默认关闭。
    bool low_latency = false;
};

// 把 `frames_per_buffer` 折算成"只收 int32 的 API"能接受的容量参数
// （当前唯一使用方：AAudio 的 `AAudioStreamBuilder_setBufferCapacityInFrames`）。
//
// 为什么需要这一层：本值是 uint32，直接 static_cast<int32_t> 会让 0x80000000 以上的请求
// 变成**负数** —— 一个"超大请求"反而退化成一个非法的小缓冲，后端要么拒开流、要么行为未
// 定义。WASAPI 侧不受影响：它在 u64 域把请求钳到引擎周期区间（见
// `wasapi_audio_playback.cpp` 的 `choose_period`），不会溢出。
// 语义与 WASAPI 对齐：**钳制**而不是拒绝 —— 请求过大是配置错误，不该让会话起不来
// （`WasapiAudioPlaybackTest.StartWithHugeFramesPerBufferSucceeds` 钉的就是这条）。
[[nodiscard]] constexpr std::int32_t frames_per_buffer_as_int32(std::uint32_t frames) noexcept
{
    constexpr auto kMax = static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    return static_cast<std::int32_t>(frames > kMax ? kMax : frames);
}

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H
