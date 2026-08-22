#ifndef AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
#define AQUA_AUDIO_BUFFER_JITTER_BUFFER_H

#include "aqua/audio/audio_frame.h"

#include <cstddef>
#include <optional>

namespace aqua::audio {

// JitterBuffer 的领域接口骨架。
//
// AudioFrame 是 Aqua 当前唯一的音频数据 block 单位，不再额外引入 AudioPacket /
// AudioStream 抽象。JitterBuffer 接受 AudioFrame 视图，但 push() 必须在实现内部
// 建立自己的数据所有权，不能长期保存 frame.data 所指向的 callback/UDP 接收缓冲。
// pop() 返回的 AudioFrame 仍是一个 view；其 data 生命周期由具体实现约定，最保守
// 的约束是：调用方应在下一次会修改 JitterBuffer 状态前消费/复制该数据。
//
// 本接口只负责网络乱序、重复、迟到与 playout 次序；不负责长期 PCM playback buffer、
// 时钟恢复或重采样。
class JitterBuffer {
public:
    virtual ~JitterBuffer() = default;

    // 插入一个已经通过 UDP/protocol 基础校验的 AudioFrame。
    // 实现必须复制/托管 frame.data，不能持有调用方的非拥有 span。
    // 返回 false 表示该 frame 被拒绝（过旧、重复、超出窗口等）。
    virtual bool push(const AudioFrame& frame) = 0;

    // 按当前 playout 策略取出一个应该交付的 AudioFrame。
    // 返回 nullopt 表示当前没有到期 frame。
    [[nodiscard]] virtual std::optional<AudioFrame> pop() = 0;

    // 当前待播放 frame 数量。
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;

    // 清空重排状态；session reset / reconnect 时使用。
    virtual void reset() noexcept = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
