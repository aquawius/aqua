#ifndef AQUA_AUDIO_DEPACKETIZER_AUDIO_DEPACKETIZER_H
#define AQUA_AUDIO_DEPACKETIZER_AUDIO_DEPACKETIZER_H

// client 侧解包器：把 UDP 收到的 Audio datagram 解码成 AudioFrame 并喂给 JitterBuffer。
//
// 数据面每个 AudioFrame 恰好一个 datagram（见 network_frame.h）。本类做 wire 解码 →
// AudioFrame 组装 → JitterBuffer::push；乱序重排 / 丢包检测由 JB 按 sequence 完成，
// 解包器不承担重排。HELLO / HelloAck 由独立的握手模块处理，本类只认 Audio。

#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aqua::audio {

class AudioDepacketizer {
public:
    // jb：目标 JitterBuffer（引用，不拥有）；frame_count：F（= 每 AudioFrame 的
    // sample frame 数，来自控制面，与 JB 的 frames_per_slot 一致）。
    AudioDepacketizer(JitterBuffer& jb, std::uint32_t frame_count);

    // 处理一个 UDP datagram。仅当 type == Audio、解码成功且 JB 接受时返回 true；
    // 非 Audio（Hello/HelloAck/Invalid）、malformed 或被 JB 拒收 → false。
    bool handle_datagram(std::span<const std::byte> datagram) noexcept;

private:
    JitterBuffer& jb_;
    std::uint32_t frame_count_;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_DEPACKETIZER_AUDIO_DEPACKETIZER_H
