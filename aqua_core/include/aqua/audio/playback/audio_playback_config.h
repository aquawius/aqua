#ifndef AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H
#define AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H

// 回放配置（值语义）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_route.h"

#include <cstdint>

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
    // 0（默认）= 后端取**设备允许的最小周期**，也就是本选项能达到的最低延迟：
    // WASAPI 用 IAudioClient3 的 minimum_period，AAudio 用系统默认 capacity
    // （2× burst）。非 0 = 请求该帧数，各后端按自己的契约折算（WASAPI 取最接近
    // 的 fundamental 倍数并钳到 [minimum, maximum]），后端实际回调粒度可以不同。
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

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H
