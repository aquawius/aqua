#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/audio/audio_format.h"
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
using aqua::audio::JitterBufferPullResult;

constexpr std::uint32_t kFrameBytes = 4; // PCM_F32LE 单声道

AudioFormat make_format()
{
    return AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
}

JitterBufferConfig make_config(std::uint32_t slots, std::uint32_t frames_per_slot)
{
    JitterBufferConfig c;
    c.capacity_slots = slots;
    c.format = make_format();
    c.frames_per_slot = frames_per_slot;
    return c;
}

bool push_frame(JitterBuffer& jb, std::uint64_t seq, std::uint32_t fps)
{
    std::vector<std::byte> data(static_cast<std::size_t>(fps) * kFrameBytes,
        static_cast<std::byte>((seq + 1) & 0xFF));
    AudioFrame f { seq, fps, std::span<const std::byte>(data) };
    return jb.push(f);
}

// 拉 K 帧，返回输出字节流的"填充值"序列（每帧取首字节，静音为 0）。
std::vector<std::uint8_t> pull_fills(JitterBuffer& jb, std::uint32_t k, std::uint32_t count)
{
    std::vector<std::uint8_t> fills;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::vector<std::byte> out(static_cast<std::size_t>(k) * kFrameBytes);
        const auto r = jb.pull(out);
        for (std::uint32_t j = 0; j < r.frames_filled; ++j) {
            fills.push_back(std::to_integer<std::uint8_t>(out[static_cast<std::size_t>(j) * kFrameBytes]));
        }
    }
    return fills;
}

TEST(JitterBufferBoundaryTest, MinCapacityOne)
{
    auto jb = JitterBuffer::create(make_config(1, 1));
    ASSERT_TRUE(jb.has_value());

    ASSERT_TRUE(push_frame(**jb, 0, 1));
    EXPECT_EQ((*jb)->used_slots(), 1u);
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 1.0);

    // 首帧即达到目标（lead=1=target），pull 出 seq 0。
    const auto fills = pull_fills(**jb, 1, 2);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0], 1u); // seq 0 数据
    EXPECT_EQ(fills[1], 0u); // 耗尽静音
    EXPECT_EQ((*jb)->used_slots(), 0u);
}

TEST(JitterBufferBoundaryTest, WaterLevelExactValues)
{
    auto jb = JitterBuffer::create(make_config(10, 1));
    ASSERT_TRUE(jb.has_value());

    // 未启动：水位 = lead/10（用 oldest/highest 计算）。
    for (std::uint64_t s = 0; s <= 2; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.3); // lead=3

    for (std::uint64_t s = 3; s <= 4; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.5); // lead=5

    ASSERT_TRUE(push_frame(**jb, 5, 1));
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.6); // lead=6

    for (std::uint64_t s = 6; s <= 8; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.9); // lead=9

    ASSERT_TRUE(push_frame(**jb, 9, 1));
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 1.0); // lead=10 满
}

TEST(JitterBufferBoundaryTest, SequenceWindowExactBoundaries)
{
    auto jb = JitterBuffer::create(make_config(10, 1));
    ASSERT_TRUE(jb.has_value());

    // 推 6 帧建 anchor（lead=6=target），pull 出 seq 0 → play_seq=1。
    for (std::uint64_t s = 0; s <= 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    pull_fills(**jb, 1, 1);

    EXPECT_FALSE(push_frame(**jb, 0, 1));  // 迟到（s < play_seq=1）
    EXPECT_FALSE(push_frame(**jb, 1, 1));  // 重复（槽 1 仍 READY）
    EXPECT_TRUE(push_frame(**jb, 6, 1));   // 正常窗内
    EXPECT_TRUE(push_frame(**jb, 10, 1));  // s == play_seq + N - 1 = 10（边界内）
    EXPECT_FALSE(push_frame(**jb, 11, 1)); // s == play_seq + N = 11（越界）
}

TEST(JitterBufferBoundaryTest, PullQuantumBoundaries)
{
    auto jb = JitterBuffer::create(make_config(30, 4)); // F=4, target=18，给足 headroom
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s <= 17; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    // anchor + 完整消费 seq 0（K=4）。
    pull_fills(**jb, 4, 1);

    // K=1：seq 1 的第 1 帧。
    EXPECT_EQ(pull_fills(**jb, 1, 1), (std::vector<std::uint8_t> { 2 }));
    // K=3：seq 1 剩余 3 帧（补完 slot 1）。
    EXPECT_EQ(pull_fills(**jb, 3, 1), (std::vector<std::uint8_t> { 2, 2, 2 }));
    // K=4：整 slot seq 2。
    EXPECT_EQ(pull_fills(**jb, 4, 1), (std::vector<std::uint8_t> { 3, 3, 3, 3 }));
    // K=5：seq 3 整 slot + seq 4 第 1 帧。
    EXPECT_EQ(pull_fills(**jb, 5, 1), (std::vector<std::uint8_t> { 4, 4, 4, 4, 5 }));
}

TEST(JitterBufferBoundaryTest, RingWrapReusesSlots)
{
    auto jb = JitterBuffer::create(make_config(4, 1)); // N=4, target=2
    ASSERT_TRUE(jb.has_value());

    ASSERT_TRUE(push_frame(**jb, 0, 1));
    ASSERT_TRUE(push_frame(**jb, 1, 1));
    // anchor + 播放 seq 0。
    EXPECT_EQ(pull_fills(**jb, 1, 1), std::vector<std::uint8_t> { 1 });

    // 每推一帧、拉一帧，保持 lead≈2；seq 4 复用 slot 0、seq 5 复用 slot 1……
    for (std::uint64_t s = 2; s <= 10; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
        // 本次 pull 播出的是 seq (s-1)，其 fill = s。
        EXPECT_EQ(pull_fills(**jb, 1, 1), std::vector<std::uint8_t> { static_cast<std::uint8_t>(s) })
            << "seq=" << s;
    }
}

TEST(JitterBufferBoundaryTest, MissingFrameSilenceAtGap)
{
    auto jb = JitterBuffer::create(make_config(30, 1)); // N=30, target=18
    ASSERT_TRUE(jb.has_value());

    // 推 0..20（缺 3），lead=21，水位保持在 normal 区经过缺失槽。
    for (std::uint64_t s = 0; s <= 20; ++s) {
        if (s == 3) {
            continue;
        }
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }

    // 前 4 帧：0,1,2,静音(缺 3)。
    const auto fills = pull_fills(**jb, 1, 4);
    ASSERT_EQ(fills.size(), 4u);
    EXPECT_EQ(fills[0], 1u);
    EXPECT_EQ(fills[1], 2u);
    EXPECT_EQ(fills[2], 3u);
    EXPECT_EQ(fills[3], 0u); // 缺帧静音
}

TEST(JitterBufferBoundaryTest, ResetClearsState)
{
    auto jb = JitterBuffer::create(make_config(10, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s <= 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    EXPECT_EQ((*jb)->used_slots(), 6u);

    (*jb)->reset();
    EXPECT_EQ((*jb)->used_slots(), 0u);
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.0);

    // 复位后重新推，从新序列开始正常。
    ASSERT_TRUE(push_frame(**jb, 100, 1));
    EXPECT_EQ((*jb)->used_slots(), 1u);
}

TEST(JitterBufferBoundaryTest, CapacityBytesExact)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    EXPECT_EQ((*jb)->capacity_slots(), 10u);
    EXPECT_EQ((*jb)->capacity_bytes(), 10u * 4u * kFrameBytes);
}

TEST(JitterBufferBoundaryTest, CustomStepFnHugeValueIsClamped)
{
    auto cfg = make_config(10, 4);
    cfg.step_fn = [](const aqua::audio::WarningStepParams&, std::uint32_t) noexcept -> std::uint32_t {
        return std::numeric_limits<std::uint32_t>::max();
    };
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    // 推 9 帧 → lead=9 → warning high（>NH=8 且 <=WH=9）。
    for (std::uint64_t s = 0; s < 9; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    std::vector<std::byte> out(4 * kFrameBytes);
    const auto r = (*jb)->pull(out);
    // UINT32_MAX 被 clamp 到 capacity(10)，而非溢出/崩溃。
    EXPECT_EQ(r.skipped_slots, 10u);
}

} // namespace
