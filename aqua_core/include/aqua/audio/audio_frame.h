#ifndef AQUA_AUDIO_FRAME_H
#define AQUA_AUDIO_FRAME_H

// AudioFrame：由 AudioBlock 重切而成的定长帧（网络 / JitterBuffer 的基本单位）。
//
// - frame_count 在单个 session 内固定（= F，由 server 经控制面下发，见
//   doc/buffer_design.md），data.size() == F × format.frame_bytes()。
// - sequence 是 audio 层的单调序号，由 AudioPacketizer 唯一产生；
//   网络乱序重排 / 丢包检测由 JitterBuffer 按 sequence 完成。
// - data 为非拥有视图，仅在产生该帧的回调生命周期内有效。

#include "aqua/audio/audio_format.h"

#include <cstdint>
#include <span>

namespace aqua::audio {

struct AudioFrame {
    std::uint64_t sequence = 0;
    std::uint32_t frame_count = 0; // 定长 F
    std::span<const std::byte> data; // F × frame_bytes 的 PCM，仅回调内有效

    [[nodiscard]] bool is_well_formed(const AudioFormat& format) const noexcept
    {
        const auto expected = format.bytes_for_frames(frame_count);
        return format.is_valid() && frame_count > 0 && expected != 0 && data.size() == expected;
    }
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_FRAME_H
