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
//   - full queue drops the newest frame;
//   - no mutex, allocation, or executor submission is performed by push().
//
// Synchronisation note:
//   push() reports a wake hint derived from the queue state immediately after publishing
//   the slot. The hint is not a synchronized queue-state fact; it only determines whether
//   the caller should attempt to wake a sleeping consumer. Every successful push must
//   advance the consumer wake generation; notify_one() is issued only when the consumer
//   cursor still points at the producer cursor value from immediately before this push.

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

class AudioFrameQueue final {
public:
    struct PushResult {
        bool accepted = false;
        bool should_notify = false;
    };

    AudioFrameQueue(std::uint32_t capacity_slots,
        std::uint32_t frame_count,
        std::uint32_t frame_bytes)
        : capacity_(capacity_slots)
        , frame_count_(frame_count)
        , frame_bytes_(frame_bytes)
        , slot_bytes_(checked_slot_bytes(frame_count, frame_bytes))
        , storage_(valid_dimensions(capacity_slots, frame_count, frame_bytes)
                ? static_cast<std::size_t>(capacity_slots) * slot_bytes_
                : 0)
        , sequences_(valid_dimensions(capacity_slots, frame_count, frame_bytes)
                ? capacity_slots
                : 0,
            0)
        , valid_(valid_dimensions(capacity_slots, frame_count, frame_bytes))
    {
    }

    AudioFrameQueue(const AudioFrameQueue&) = delete;
    AudioFrameQueue& operator=(const AudioFrameQueue&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] PushResult push(const AudioFrame& frame) noexcept
    {
        if (!valid_ || frame.frame_count != frame_count_
            || frame.data.size() != slot_bytes_) {
            return {};
        }

        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const std::uint32_t index = static_cast<std::uint32_t>(head % capacity_);
        auto* dst = storage_.data() + static_cast<std::size_t>(index) * slot_bytes_;
        std::copy_n(frame.data.data(), slot_bytes_, dst);
        sequences_[index] = frame.sequence;

        // Publish the fully written slot only after payload + metadata are visible.
        head_.store(head + 1, std::memory_order_release);

        // Re-read the consumer cursor after publication. The pre-publish snapshot can
        // become stale while the slot is being copied (the consumer may drain the old
        // backlog in that window). Only the post-publication observation tells us
        // whether this push is still the first outstanding item that may need to wake
        // a sleeping consumer.
        const bool should_notify =
            tail_.load(std::memory_order_acquire) == head;
        return { true, should_notify };
    }

    // Consumer 必须为 nothrow、non-blocking、non-allocating 的实时安全 callable；
    // 编译期强制 nothrow。
    template <typename Consumer>
    bool consume_one(Consumer&& consumer) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Consumer&, const AudioFrame&>,
            "AudioFrameQueue consumer must be noexcept");
        if (!valid_) {
            return false;
        }

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
        return !valid_ || head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint32_t capacity_slots() const noexcept { return capacity_; }

    [[nodiscard]] std::uint32_t size_slots() const noexcept
    {
        if (!valid_) {
            return 0;
        }
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
    static constexpr bool valid_dimensions(
        std::uint32_t capacity_slots,
        std::uint32_t frame_count,
        std::uint32_t frame_bytes) noexcept
    {
        if (capacity_slots == 0 || frame_count == 0 || frame_bytes == 0) {
            return false;
        }
        const auto frame_count_size = static_cast<std::size_t>(frame_count);
        if (frame_count_size > std::numeric_limits<std::size_t>::max() / frame_bytes) {
            return false;
        }
        const auto slot_bytes = frame_count_size * frame_bytes;
        return static_cast<std::size_t>(capacity_slots)
            <= std::numeric_limits<std::size_t>::max() / slot_bytes;
    }

    static constexpr std::size_t checked_slot_bytes(
        std::uint32_t frame_count, std::uint32_t frame_bytes) noexcept
    {
        if (frame_count == 0 || frame_bytes == 0) {
            return 0;
        }
        const auto count = static_cast<std::size_t>(frame_count);
        if (count > std::numeric_limits<std::size_t>::max() / frame_bytes) {
            return 0;
        }
        return count * frame_bytes;
    }

    const std::uint32_t capacity_;
    const std::uint32_t frame_count_;
    const std::uint32_t frame_bytes_;
    const std::size_t slot_bytes_;
    const bool valid_;

    std::vector<std::byte> storage_;
    std::vector<std::uint64_t> sequences_;

    alignas(64) std::atomic<std::uint64_t> head_ { 0 }; // producer-owned
    alignas(64) std::atomic<std::uint64_t> tail_ { 0 }; // consumer-owned
    std::atomic<std::uint64_t> dropped_ { 0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H
