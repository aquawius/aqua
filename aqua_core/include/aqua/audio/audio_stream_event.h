#ifndef AQUA_AUDIO_AUDIO_STREAM_EVENT_H
#define AQUA_AUDIO_AUDIO_STREAM_EVENT_H

// 音频流事件的归属契约（backend <-> manager 边界）。
//
// manager 复用同一个 backend 实例跨越多次 stop/start（切换事务就是
// stop -> start，不重建 backend）。因此一个不带来源的 AudioError 在上层是
// 不可判别的：它可能是「我刚起的流死了」，也可能是「上一条流的迟到讣告」。
// 两者要求的响应完全相反——前者要恢复，后者要忽略。
//
// 所以事件必须带 generation：backend 每次成功 start() 递增，回调只允许
// 上报自己那条流的 generation。manager 据此判别归属，不再靠
// PlaybackState::Switching 之类的时间窗猜测（错误可能在新流已启动后才到）。
//
// 各 backend 的 generation 来源：
//   AAudio  —— error callback 与 close() 不同步，迟到的路由断开事件是真实
//              存在的；generation 由 per-stream 槽位携带（见
//              aaudio_audio_playback.h 的 StreamSlot）。
//   WASAPI —— 事件线程由 backend 自己持有并 join，天然不会跨流；仍然照契约
//              打戳，让 manager 侧的判别逻辑与平台无关。

#include "aqua/audio/audio_error.h"
#include "aqua/compat/move_only_function.h"

#include <cstdint>

namespace aqua::audio {

// 流代号。0 保留给「没有流」，第一条流从 1 开始。
using StreamGeneration = std::uint64_t;
inline constexpr StreamGeneration kNoStreamGeneration = 0;

struct AudioStreamEvent {
    AudioError error = AudioError::None;
    StreamGeneration generation = kNoStreamGeneration;

    // 设备类丢失（拔出 / 不可用 / 消失）：走恢复事务。
    // 其余错误（格式、后端内部）保持既有 Degraded 语义。
    [[nodiscard]] bool is_device_loss() const noexcept
    {
        return error == AudioError::DeviceDisconnected
            || error == AudioError::DeviceUnavailable
            || error == AudioError::DeviceNotFound;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return error == AudioError::None;
    }
};

// 运行期事件回调。运行在 backend 的内部线程（已退出实时音频数据路径），
// 不是调用 start()/stop() 的控制线程，实现必须快速返回；不得在其中调用
// backend 的 stop()/start()（会 join 自身导致死锁）。
using AudioStreamEventCallback
    = compat::MoveOnlyFunction<void(const AudioStreamEvent&) noexcept>;

} // namespace aqua::audio

#endif // AQUA_AUDIO_AUDIO_STREAM_EVENT_H
