#ifndef AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
#define AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H

// AudioPacketizer：capture 侧的固定帧重切器。
//
// 实时契约：push() 只进行有界内存拷贝和用户提供的同步 sink 调用；
// 本类在构造期一次性分配 pending storage，push() 本身不分配、不加锁、不做 IO。
// sink 必须满足同样的 realtime contract。

#include "aqua/audio/audio_frame.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
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
        static_assert(std::is_nothrow_invocable_v<Sink&, const AudioFrame&>,
            "AudioPacketizer sink must be noexcept");
        if (!valid()) {
            return;
        }
        if (pcm.empty()) {
            return;
        }
        input_blocks_.fetch_add(1, std::memory_order_relaxed);
        input_bytes_.fetch_add(pcm.size(), std::memory_order_relaxed);
        if (pcm.size() % frame_bytes_ != 0) {
            rejected_unaligned_blocks_.fetch_add(1, std::memory_order_relaxed);
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
                .sequence = sequence_.load(std::memory_order_relaxed),
                .frame_count = frame_count_,
                .data = std::span<const std::byte>(pending_.data(), chunk),
            });
            sequence_.fetch_add(1, std::memory_order_relaxed);
            pending_size_ = 0;
        }
    }

    [[nodiscard]] std::uint64_t input_blocks() const noexcept { return input_blocks_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t input_bytes() const noexcept { return input_bytes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_emitted() const noexcept { return sequence_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t rejected_unaligned_blocks() const noexcept
    {
        return rejected_unaligned_blocks_.load(std::memory_order_relaxed);
    }

    // 仅允许在 capture producer 停止后调用。
    void reset() noexcept;

private:
    std::uint32_t frame_bytes_;
    std::uint32_t frame_count_;
    std::vector<std::byte> pending_;
    std::size_t pending_size_ = 0;
    std::atomic<std::uint64_t> sequence_ { 0 };
    std::atomic<std::uint64_t> input_blocks_ { 0 };
    std::atomic<std::uint64_t> input_bytes_ { 0 };
    std::atomic<std::uint64_t> rejected_unaligned_blocks_ { 0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PACKETIZER_AUDIO_PACKETIZER_H
