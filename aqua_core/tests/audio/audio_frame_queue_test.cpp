#include "aqua/audio/queue/audio_frame_queue.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace {

using aqua::audio::AudioFrame;
using aqua::audio::AudioFrameQueue;

std::array<std::byte, 16> payload(std::uint8_t value)
{
    std::array<std::byte, 16> out {};
    out.fill(static_cast<std::byte>(value));
    return out;
}

TEST(AudioFrameQueueTest, FirstPushReportsEmptyToNonEmptyTransition)
{
    AudioFrameQueue queue(4, 4, 4);
    const auto bytes = payload(1);

    const auto first = queue.push(AudioFrame { 10, 4, bytes });
    const auto second = queue.push(AudioFrame { 11, 4, bytes });

    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.was_empty);
    EXPECT_TRUE(second.accepted);
    EXPECT_FALSE(second.was_empty);
    EXPECT_EQ(queue.size_slots(), 2u);
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

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!queue.push(AudioFrame { i, 4, bytes }).accepted) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (!producer_done.load(std::memory_order_acquire) || !queue.empty()) {
            if (!queue.consume_one([&](const AudioFrame& frame) noexcept {
                    if (frame.sequence != expected || frame.data.size() != bytes.size()) {
                        failed.store(true, std::memory_order_release);
                    }
                    ++expected;
                    consumed.store(expected, std::memory_order_relaxed);
                })) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load(std::memory_order_acquire));
    EXPECT_EQ(consumed.load(std::memory_order_relaxed), kCount);
    EXPECT_EQ(queue.size_slots(), 0u);
    EXPECT_EQ(queue.dropped_frames(), 0u);
}

} // namespace
