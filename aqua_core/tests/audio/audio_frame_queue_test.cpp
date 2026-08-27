#include "aqua/audio/queue/audio_frame_queue.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <limits>

namespace {

using aqua::audio::AudioFrame;
using aqua::audio::AudioFrameQueue;

std::array<std::byte, 16> payload(std::uint8_t value)
{
    std::array<std::byte, 16> out {};
    out.fill(static_cast<std::byte>(value));
    return out;
}

TEST(AudioFrameQueueTest, FirstPushRequestsWakeAndFollowingPushDoesNot)
{
    AudioFrameQueue queue(4, 4, 4);
    const auto bytes = payload(1);

    const auto first = queue.push(AudioFrame { 10, 4, bytes });
    const auto second = queue.push(AudioFrame { 11, 4, bytes });

    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.should_notify);
    EXPECT_TRUE(second.accepted);
    EXPECT_FALSE(second.should_notify);
    EXPECT_EQ(queue.size_slots(), 2u);
}

TEST(AudioFrameQueueTest, RejectsInvalidDimensions)
{
    EXPECT_FALSE(AudioFrameQueue(0, 4, 4).valid());
    EXPECT_FALSE(AudioFrameQueue(4, 0, 4).valid());
    EXPECT_FALSE(AudioFrameQueue(4, 4, 0).valid());
    EXPECT_FALSE(AudioFrameQueue(
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max()).valid());
}

TEST(AudioFrameQueueTest, FullQueueDropsNewestFrame)
{
    AudioFrameQueue queue(2, 4, 4);
    const auto bytes = payload(2);

    EXPECT_TRUE(queue.push(AudioFrame { 1, 4, bytes }).accepted);
    EXPECT_TRUE(queue.push(AudioFrame { 2, 4, bytes }).accepted);
    const auto dropped = queue.push(AudioFrame { 3, 4, bytes });

    EXPECT_FALSE(dropped.accepted);
    EXPECT_EQ(queue.dropped_frames(), 1u);
    EXPECT_EQ(queue.size_slots(), 2u);
}

TEST(AudioFrameQueueTest, ConcurrentSingleProducerSingleConsumerPreservesOrder)
{
    constexpr std::uint64_t kCount = 200000;
    AudioFrameQueue queue(32, 4, 4);
    const auto bytes = payload(3);
    std::atomic<bool> producer_done { false };
    std::atomic<std::uint64_t> consumed { 0 };
    std::atomic<bool> failed { false };

    // 生产者与生产语义一致：push 一次即继续，队列满则丢帧（不重试）。
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            (void)queue.push(AudioFrame { i, 4, bytes });
        }
        producer_done.store(true, std::memory_order_release);
    });

    // 消费者校验：消费到的 sequence 必须严格递增（允许因丢帧产生的 gap）。
    std::thread consumer([&] {
        std::uint64_t last_sequence = 0;
        bool first = true;
        while (!producer_done.load(std::memory_order_acquire) || !queue.empty()) {
            if (!queue.consume_one([&](const AudioFrame& frame) noexcept {
                    if (frame.data.size() != bytes.size()
                        || (!first && frame.sequence <= last_sequence)) {
                        failed.store(true, std::memory_order_release);
                    }
                    first = false;
                    last_sequence = frame.sequence;
                    consumed.fetch_add(1, std::memory_order_relaxed);
                })) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    // 每帧要么被消费、要么被丢（满队列拒绝），二者之和必须等于总帧数。
    EXPECT_EQ(consumed.load(std::memory_order_relaxed) + queue.dropped_frames(), kCount);
    EXPECT_EQ(queue.size_slots(), 0u);
}

} // namespace
