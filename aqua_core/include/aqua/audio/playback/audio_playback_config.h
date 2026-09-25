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
    // 0 表示由后端按设备/低延迟策略自行决定；后端实际回调粒度可以不同。
    std::uint32_t frames_per_buffer = 480;

    // 是否请求低延迟播放路径。Android/AAudio 使用此选项选择
    // LOW_LATENCY + SHARED 或 NONE + SHARED；其它后端应忽略。默认关闭。
    bool low_latency = false;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_AUDIO_PLAYBACK_CONFIG_H
