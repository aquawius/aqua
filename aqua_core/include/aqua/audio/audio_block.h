#ifndef AQUA_AUDIO_BLOCK_H
#define AQUA_AUDIO_BLOCK_H

// AudioBlock：采集后端产出的原始 PCM 数据块（变长）。
//
// 采集后端一次回调给出的块大小不固定（如 WASAPI 一次事件含多个 packet），
// 由 AudioPacketizer 攒块并重切为定长 AudioFrame（见 audio_frame.h）。
// data 为非拥有视图，仅在产生该 block 的回调生命周期内有效。

#include <cstddef>
#include <span>

namespace aqua::audio {

struct AudioBlock {
    std::span<const std::byte> data; // 变长 PCM，仅回调内有效
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BLOCK_H
