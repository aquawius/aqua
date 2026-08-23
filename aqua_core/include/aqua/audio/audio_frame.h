#ifndef AQUA_AUDIO_FRAME_H
#define AQUA_AUDIO_FRAME_H

// 实时音频管线的数据 block。
//
// 这里的“frame”指一段连续 PCM block，而不是单个 sample frame。
// data 中包含 frame_count 个 sample frame；每个 sample frame 由所有声道组成。
// data 为非拥有视图，仅在产生该 frame 的回调生命周期内有效。

#include "aqua/audio/audio_format.h"
#include <cstddef>

#include <cstdint>
#include <span>

namespace aqua::audio {

struct AudioFrame {
    // 同一音频流内单调递增。帧粒度由采集/网络层自行决定，不承诺一个 AudioFrame 恰好对应一个网络包。
    std::uint64_t sequence = 0;

    // 该 block 在 backend 使用的单调音频时间线上的位置，单位为纳秒。
    // clock domain 由 backend 定义；不得将其解释为 Unix wall-clock，
    // 也不得在没有时钟同步的情况下直接用于跨机器比较。
    std::uint64_t timestamp_ns = 0;

    // data 中包含多少个 sample frame（每个 frame 包含全部 channels）。
    std::uint32_t frame_count = 0;

    // PCM block；仅在当前 callback 内有效。
    std::span<const std::byte> data;

    [[nodiscard]] bool is_well_formed(const AudioFormat& format) const noexcept
    {
        const auto expected = format.bytes_for_frames(frame_count);
        return format.is_valid() && frame_count > 0 && expected != 0 && data.size() == expected;
    }
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_FRAME_H
