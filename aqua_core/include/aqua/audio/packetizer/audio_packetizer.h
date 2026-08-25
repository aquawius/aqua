#ifndef AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
#define AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H

// server 侧打包器：把 capture 输出的变长 PCM block 重新打包成固定大小 AudioFrame。
//
// capture 后端一次回调可能给出不定长的 PCM（如 WASAPI 一次事件含多个 packet），
// 而数据面要求每个 AudioFrame 固定 `frame_count` 个 sample frame（= F，与 client
// JitterBuffer 的 `frames_per_slot` 一致，见 doc/buffer_design.md §15）。本类负责：
//   1) 攒变长输入，2) 每凑满 F 帧切出一个 AudioFrame，3) 赋单调递增 sequence。
// 编码成 wire 包 / UDP 发送由上层（runtime）在回调里完成。

#include "aqua/audio/audio_format.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aqua::audio {

class AudioPacketizer {
public:
    // 每凑满一个 AudioFrame 回调一次。sequence 单调递增（从 0 起）；
    // pcm 视图指向内部缓冲，仅在回调内有效（禁止跨回调持有）。
    using FrameHandler = void (*)(void* user_data, std::uint64_t sequence,
        std::span<const std::byte> pcm) noexcept;

    // frame_count：每 AudioFrame 的 sample frame 数（= AudioFrame::frame_count = F）；
    // frame_bytes：一个 sample frame 的字节数。两者必须 > 0（由上层保证）。
    AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes);

    // 配置合法性：frame_count 与 frame_bytes 均必须 > 0（否则 chunk 为 0，push 静默丢弃）。
    [[nodiscard]] static bool is_valid_config(std::uint32_t frame_count, std::uint32_t frame_bytes) noexcept;

    // 本实例配置是否合法（等价于 is_valid_config(frame_count_, frame_bytes_)）。
    [[nodiscard]] bool valid() const noexcept;

    // 喂入一段 PCM（必须 frame-aligned，即 size 为 frame_bytes 的整数倍）。
    // 每攒满 F 帧调用一次 handler；不保证本函数内恰好调用 handler（可能 0 次或多次）。
    // noexcept：分配失败时丢弃余量并返回，保证可安全用于实时回调。
    void push(std::span<const std::byte> pcm, FrameHandler handler, void* user_data) noexcept;

    // 已切出的 AudioFrame 数（= 下一个将使用的 sequence）。
    [[nodiscard]] std::uint64_t frames_emitted() const noexcept { return sequence_; }

    // 清空未凑满的余量与 sequence 计数（停止采集后复用）。
    void reset() noexcept;

private:
    std::uint32_t frame_bytes_;
    std::uint32_t frame_count_;
    std::vector<std::byte> pending_; // 未凑满 F 帧的余量（< F×frame_bytes）
    std::uint64_t sequence_ = 0;
};

// 由 MTU payload 预算反推每 AudioFrame 的 sample frame 数 F：见 audio_format.h 的
// frame_count_for_budget（放 base 以便 server/client 共用）。

} // namespace aqua::audio

#endif // AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
