#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/audio_frame.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;

constexpr std::uint32_t kFrameBytes = 4; // PCM_F32LE 单声道

AudioFormat format()
{
    return AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
}

JitterBufferConfig config(std::uint32_t capacity, std::uint32_t frame_count)
{
    JitterBufferConfig c;
    c.capacity_slots = capacity;
    c.format = format();
    c.frame_count = frame_count;
    return c;
}

bool push_frame(JitterBuffer& jb, std::uint64_t seq, std::uint32_t frame_count = 1)
{
    std::vector<std::byte> data(static_cast<std::size_t>(frame_count) * kFrameBytes,
        static_cast<std::byte>((seq + 1) & 0xFF));
    return jb.push(AudioFrame {
        .sequence = seq,
        .frame_count = frame_count,
        .data = std::span<const std::byte>(data),
    });
}

void pull_one(JitterBuffer& jb, std::vector<std::byte>& out)
{
    const auto result = jb.pull(out);
    ASSERT_EQ(result.frames_filled, 1u);
}

TEST(JitterBufferRecoveryRegressionTest,
    ReanchorCleansStaleReadySlotsAndPreservesUsedSlotAccounting)
{
    auto jb = JitterBuffer::create(config(30, 1));
    ASSERT_TRUE(jb.has_value());

    // 建立活动时间线，并在 play_seq=1 之后留下 READY 积压。
    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_one(**jb, out);

    ASSERT_EQ((*jb)->used_slots(), 17u);
    EXPECT_FALSE(push_frame(**jb, 31));
    // 31 % 30 == 1，触发帧与仍为 READY 的 seq=1 同槽碰撞而被拒绝；
    // 但远超前请求仍必须保持 pending。
    EXPECT_EQ((*jb)->used_slots(), 17u);
    EXPECT_EQ((*jb)->reanchor_count(), 0u);

    // 现有播放机制先消费剩余的正常积压，然后进入低水位 Hold；
    // 连续若干次 Hold 无变化后，延迟的 reanchor 请求才被应用。
    for (int i = 0; i < 12 && (*jb)->reanchor_count() == 0; ++i) {
        pull_one(**jb, out);
    }

    ASSERT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 31u);
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

TEST(JitterBufferRecoveryRegressionTest,
    FarthestPendingReanchorWinsWhenRequestsArriveInReverseOrder)
{
    auto jb = JitterBuffer::create(config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_one(**jb, out); // play=1，highest=17

    // 较小的请求先到，较远的请求后到；两者的目标槽都为空。
    ASSERT_TRUE(push_frame(**jb, 48));
    ASSERT_TRUE(push_frame(**jb, 60));

    pull_one(**jb, out);

    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 60u);
    // 60 位于新窗口内被保留，48 已过期必须被清扫。
    EXPECT_EQ((*jb)->used_slots(), 1u);
}

TEST(JitterBufferRecoveryRegressionTest,
    FarthestPendingReanchorWinsEvenWhenSmallerRequestArrivesLast)
{
    auto jb = JitterBuffer::create(config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_one(**jb, out);

    ASSERT_TRUE(push_frame(**jb, 60));
    ASSERT_TRUE(push_frame(**jb, 48));
    pull_one(**jb, out);

    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 60u);
}

TEST(JitterBufferRecoveryRegressionTest,
    ReanchorAppliesOnlyOnceUntilANewRequestArrives)
{
    auto jb = JitterBuffer::create(config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_one(**jb, out);

    ASSERT_TRUE(push_frame(**jb, 60));
    pull_one(**jb, out);
    ASSERT_EQ((*jb)->reanchor_count(), 1u);
    ASSERT_EQ((*jb)->last_reanchor_sequence(), 60u);

    for (int i = 0; i < 8; ++i) {
        pull_one(**jb, out);
    }
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 60u);
}

TEST(JitterBufferRecoveryRegressionTest,
    ReanchorTimelineResumesNormallyAfterTargetFill)
{
    auto jb = JitterBuffer::create(config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_one(**jb, out);

    ASSERT_TRUE(push_frame(**jb, 60));
    pull_one(**jb, out); // 应用 reanchor；进入 Hold 状态
    ASSERT_EQ((*jb)->reanchor_count(), 1u);

    // 从新锚点开始，恰好填到 normal target。
    for (std::uint64_t seq = 61; seq < 78; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq));
    }

    const auto resumed = (*jb)->pull(out);
    ASSERT_EQ(resumed.frames_filled, 1u);
    EXPECT_EQ(resumed.silence_frames, 0u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0]),
        static_cast<std::uint8_t>((60 + 1) & 0xFF));
}

TEST(JitterBufferRecoveryRegressionTest, RejectsReservedSequenceSentinel)
{
    auto jb = JitterBuffer::create(config(4, 1));
    ASSERT_TRUE(jb.has_value());

    constexpr std::uint64_t kSentinel = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(push_frame(**jb, kSentinel));
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

} // namespace
