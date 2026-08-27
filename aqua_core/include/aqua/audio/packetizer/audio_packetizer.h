#ifndef AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
#define AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H

// AudioPacketizer：capture 侧的固定帧重切器。
//
// 实时契约：push() 只进行有界内存拷贝和用户提供的同步 sink 调用；
// 本类在构造期一次性分配 pending storage，push() 本身不分配、不加锁、不做 IO。
// sink 必须满足同样的 realtime contract。

#include "aqua/audio/audio_frame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace aqua::audio {

class AudioPacketizer final {
public:
    AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes);

    [[nodiscard]] static bool is_valid_config(
        std::uint32_t frame_count, std::uint32_t frame_bytes) noexcept;
    [[nodiscard]] bool valid() const noexcept;

    // pcm 必须按 sample-frame 对齐。sink 在本次调用内同步执行，frame.data
    // 仅在 sink 返回前有效。sink 不得阻塞、分配或抛异常。
    template <typename Sink>
    void push(std::span<const std::byte> pcm, Sink&& sink) noexcept
    {
        if (!valid() || pcm.empty() || pcm.size() % frame_bytes_ != 0) {
            return;
        }

        const std::size_t chunk = pending_.size();
        std::size_t offset = 0;

        while (offset < pcm.size()) {
            const std::size_t to_copy = std::min(chunk - pending_size_, pcm.size() - offset);
            std::copy_n(pcm.data() + offset, to_copy, pending_.data() + pending_size_);
            pending_size_ += to_copy;
            offset += to_copy;

            if (pending_size_ != chunk) {
                continue;
            }

            sink(AudioFrame {
                .sequence = sequence_,
                .frame_count = frame_count_,
                .data = std::span<const std::byte>(pending_.data(), chunk),
            });
            ++sequence_;
            pending_size_ = 0;
        }
    }

    [[nodiscard]] std::uint64_t frames_emitted() const noexcept { return sequence_; }

    // 仅允许在 capture producer 停止后调用。
    void reset() noexcept;

private:
    std::uint32_t frame_bytes_;
    std::uint32_t frame_count_;
    std::vector<std::byte> pending_;
    std::size_t pending_size_ = 0;
    std::uint64_t sequence_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
