#ifndef AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H
#define AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H

// Fixed-capacity SPSC AudioFrame handoff.
//
// This queue is intentionally narrower than JitterBuffer:
//   capture RT thread -> network worker thread
//
// Properties:
//   - capacity and storage are allocated at construction time only;
//   - producer performs only atomic loads/stores and a bounded memcpy;
//   - consumer owns a slot until the supplied callback returns;
//   - full queue drops the newest frame; capacity is intentionally tiny so a
//     temporary network stall cannot turn this handoff into a long-latency audio buffer;
//   - no mutex, allocation, or executor submission is performed by push().

#include "aqua/audio/audio_frame.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace aqua::audio {

class AudioFrameQueue final {
public:
    struct PushResult {
        bool accepted = false;
        bool was_empty = false;
    };

    AudioFrameQueue(std::uint32_t capacity_slots,
        std::uint32_t frame_count,
        std::uint32_t frame_bytes)
        : capacity_(capacity_slots)
        , frame_count_(frame_count)
        , frame_bytes_(frame_bytes)
        , slot_bytes_(static_cast<std::size_t>(frame_count) * frame_bytes)
        , storage_(static_cast<std::size_t>(capacity_slots) * slot_bytes_)
        , sequences_(capacity_slots, 0)
    {
    }

    AudioFrameQueue(const AudioFrameQueue&) = delete;
    AudioFrameQueue& operator=(const AudioFrameQueue&) = delete;

    [[nodiscard]] PushResult push(const AudioFrame& frame) noexcept
    {
        if (capacity_ == 0 || frame.frame_count != frame_count_
            || frame.data.size() != slot_bytes_) {
            return {};
        }

        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const bool was_empty = head == tail;
        const std::uint32_t index = static_cast<std::uint32_t>(head % capacity_);
        auto* dst = storage_.data() + static_cast<std::size_t>(index) * slot_bytes_;
        std::copy_n(frame.data.data(), slot_bytes_, dst);
        sequences_[index] = frame.sequence;

        // Publish the fully written slot only after payload + metadata are visible.
        head_.store(head + 1, std::memory_order_release);
        return { true, was_empty };
    }

    template <typename Consumer>
    bool consume_one(Consumer&& consumer) noexcept
    {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }

        const std::uint32_t index = static_cast<std::uint32_t>(tail % capacity_);
        const auto* src = storage_.data() + static_cast<std::size_t>(index) * slot_bytes_;
        const AudioFrame frame {
            .sequence = sequences_[index],
            .frame_count = frame_count_,
            .data = std::span<const std::byte>(src, slot_bytes_),
        };

        consumer(frame);

        // Producer may reuse the slot only after consumer has finished reading it.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint32_t capacity_slots() const noexcept { return capacity_; }

    [[nodiscard]] std::uint32_t size_slots() const noexcept
    {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto size = head - tail;
        return size > capacity_ ? capacity_ : static_cast<std::uint32_t>(size);
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    const std::uint32_t capacity_;
    const std::uint32_t frame_count_;
    const std::uint32_t frame_bytes_;
    const std::size_t slot_bytes_;

    std::vector<std::byte> storage_;
    std::vector<std::uint64_t> sequences_;

    // Monotonic cursors are intentionally allowed to wrap only at uint64 overflow,
    // which is practically unreachable for this long-running audio service.
    alignas(64) std::atomic<std::uint64_t> head_ { 0 }; // producer-owned
    alignas(64) std::atomic<std::uint64_t> tail_ { 0 }; // consumer-owned
    std::atomic<std::uint64_t> dropped_ { 0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H
