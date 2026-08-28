#ifndef AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H
#define AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H

// 固定容量的 SPSC AudioFrame 交接队列。
//
// 该队列有意比 JitterBuffer 更窄：
//   capture RT 线程 -> network worker 线程
//
// 特性：
//   - 容量与存储仅在构造期分配；
//   - producer 只做原子 load/store 与一次有界 memcpy；
//   - consumer 占有某个槽，直到传入的回调返回；
//   - 队列满时丢弃最新帧；
//   - push() 不发生互斥、堆分配或 executor 提交。
//
// 同步说明：
//   push() 在发布槽位后立刻依据队列状态给出唤醒提示。该提示不是同步后的
//   队列状态事实，只决定调用方是否应尝试唤醒休眠的 consumer。每次成功 push
//   都必须推进 consumer 唤醒 generation；只有当 consumer 游标仍指向本次 push
//   之前那一刻的 producer 游标值时，才发出 notify_one()。

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

    [[nodiscard]] std::uint32_t capacity_slots() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t frame_count() const noexcept { return frame_count_; }
    [[nodiscard]] std::uint32_t frame_bytes() const noexcept { return frame_bytes_; }
    [[nodiscard]] std::size_t slot_bytes() const noexcept { return slot_bytes_; }

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

        // 只有当 payload 与元数据都可见后，才发布写完整的槽。
        head_.store(head + 1, std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);

        // 发布后再读一次 consumer 游标。发布前的快照在拷贝槽期间可能已过期
        // （consumer 可能在那段时间排空旧积压）。只有发布后的观测才能确定
        // 本次 push 是否仍是第一个可能需要唤醒休眠 consumer 的未处理项。
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

        // 只有当 consumer 读完该槽后，producer 才能复用它。
        tail_.store(tail + 1, std::memory_order_release);
        consumed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return !valid_ || head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

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

    [[nodiscard]] std::uint64_t accepted_frames() const noexcept
    {
        return accepted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t consumed_frames() const noexcept
    {
        return consumed_.load(std::memory_order_relaxed);
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

    alignas(64) std::atomic<std::uint64_t> head_ { 0 }; // producer 持有
    alignas(64) std::atomic<std::uint64_t> tail_ { 0 }; // consumer 持有
    std::atomic<std::uint64_t> accepted_ { 0 };
    std::atomic<std::uint64_t> consumed_ { 0 };
    std::atomic<std::uint64_t> dropped_ { 0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_QUEUE_AUDIO_FRAME_QUEUE_H
