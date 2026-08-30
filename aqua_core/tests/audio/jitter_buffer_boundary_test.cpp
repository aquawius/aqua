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

JitterBufferConfig make_config(std::uint32_t slots, std::uint32_t frame_count)
{
    JitterBufferConfig c;
    c.capacity_slots = slots;
    c.format = make_format();
    c.frame_count = frame_count;
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

TEST(JitterBufferBoundaryTest, MinCapacityIsFour)
{
    // 容量 < 4 被拒绝：整数水位量化会让 warning/normal 区间塌缩为 0 slot。
    EXPECT_FALSE(JitterBuffer::create(make_config(1, 1)).has_value());
    EXPECT_FALSE(JitterBuffer::create(make_config(3, 1)).has_value());

    // 最小合法容量为 4。
    auto jb = JitterBuffer::create(make_config(4, 1));
    ASSERT_TRUE(jb.has_value());
    EXPECT_EQ((*jb)->capacity_slots(), 4u);

    ASSERT_TRUE(push_frame(**jb, 0, 1));
    EXPECT_EQ((*jb)->used_slots(), 1u);
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

    EXPECT_FALSE(push_frame(**jb, 0, 1)); // 迟到（s < play_seq=1）
    EXPECT_FALSE(push_frame(**jb, 1, 1)); // 重复（槽 1 仍 READY）
    EXPECT_TRUE(push_frame(**jb, 6, 1)); // 正常窗内
    EXPECT_TRUE(push_frame(**jb, 10, 1)); // s == play_seq + N - 1 = 10（边界内）
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

TEST(JitterBufferBoundaryTest, DefaultWarningStepUsesGentleCappedCurve)
{
    aqua::audio::WarningStepParams p;
    p.min_step = 1;
    p.max_step = 3;
    p.growth = 2.0;

    EXPECT_EQ(aqua::audio::default_warning_step(p, 1), 1u);
    EXPECT_EQ(aqua::audio::default_warning_step(p, 4), 1u);
    EXPECT_EQ(aqua::audio::default_warning_step(p, 5), 2u);
    EXPECT_EQ(aqua::audio::default_warning_step(p, 8), 2u);
    EXPECT_EQ(aqua::audio::default_warning_step(p, 9), 3u);
    EXPECT_EQ(aqua::audio::default_warning_step(p, 100), 3u);
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

TEST(JitterBufferBoundaryTest, RejectsInvalidStepRange)
{
    auto cfg = make_config(30, 4);
    cfg.step.min_step = 4;
    cfg.step.max_step = 2;
    EXPECT_FALSE(aqua::audio::JitterBuffer::create(cfg).has_value());
}

TEST(JitterBufferBoundaryTest, FullWindowSequentialNextFrameDoesNotRequestReanchor)
{
    auto cfg = make_config(4, 1);
    cfg.normal_high = 0.90; // lead=4（满窗）不触发高水位 DROP
    cfg.warning_high = 0.99;
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 4; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out); // play=1, highest=3
    ASSERT_TRUE(push_frame(**jb, 4, 1)); // full window: play=1, highest=4
    ASSERT_EQ((*jb)->used_slots(), 4u);

    // seq=5 is exactly the next sequential frame. Its slot is busy, but this
    // normal full-window condition must not manufacture a reanchor request.
    EXPECT_FALSE(push_frame(**jb, 5, 1));
    EXPECT_EQ((*jb)->reanchor_requests(), 0u);
    EXPECT_EQ((*jb)->reanchor_count(), 0u);
}

TEST(JitterBufferBoundaryTest, PreStartFullWindowSequentialNextFrameDoesNotRequestReanchor)
{
    auto jb = JitterBuffer::create(make_config(4, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 4; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq, 1));
    }
    ASSERT_EQ((*jb)->used_slots(), 4u);

    EXPECT_FALSE(push_frame(**jb, 4, 1));
    EXPECT_EQ((*jb)->reanchor_requests(), 0u);
    EXPECT_EQ((*jb)->reanchor_count(), 0u);
}

TEST(JitterBufferBoundaryTest, ReanchorAppliesWhenHoldIsAtLiveEdge)
{
    auto cfg = make_config(4, 1);
    cfg.warning_low = 0.40;
    cfg.normal_low = 0.55;
    cfg.target = 0.75;
    cfg.normal_high = 0.85;
    cfg.warning_high = 0.95;
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    ASSERT_TRUE(push_frame(**jb, 0, 1));
    ASSERT_TRUE(push_frame(**jb, 1, 1));
    ASSERT_TRUE(push_frame(**jb, 2, 1));
    std::vector<std::byte> out(2 * kFrameBytes);
    (*jb)->pull(out); // play=2，highest=2，低水位 Hold 已武装

    EXPECT_FALSE(push_frame(**jb, 10, 1)); // 当前槽仍被占用而拒绝；但请求被保留
    EXPECT_EQ((*jb)->reanchor_count(), 0u);
    (*jb)->pull(std::span<std::byte>(out.data(), kFrameBytes));
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 10u);
}

TEST(JitterBufferBoundaryTest, ReanchorEscapesPartialBurstGap)
{
    auto jb = JitterBuffer::create(make_config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s < 6; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out); // play=1，highest=5
    ASSERT_TRUE(push_frame(**jb, 36, 1)); // 远超前触发帧；请求被延迟

    // 延迟的缺口已跨越整个接收窗口，恢复过程不得用 O(gap/capacity) 次 pull
    // 去跳过一段人为的空时间线；请求可见后应在第一次消费 pull 即被应用。
    const auto recovery = (*jb)->pull(out);
    EXPECT_EQ(recovery.silence_frames, 1u);
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 36u);
}

TEST(JitterBufferBoundaryTest, ReanchorRetainsTriggerFrame)
{
    auto jb = JitterBuffer::create(make_config(4, 1));
    ASSERT_TRUE(jb.has_value());

    // 建立播放时间线：填到 target 后 pull 一帧，推进 play_seq，
    // 同时在新时间线中保留触发槽。
    for (std::uint64_t s = 0; s < 3; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    pull_fills(**jb, 1, 1); // 锚定在 seq 0 → play=1

    // seq=7 远超前于 play=1（距离 6 >= N=4），且落在空槽上。
    // 它既是 reanchor 触发帧，也是新时间线里真实的 READY 槽。
    ASSERT_TRUE(push_frame(**jb, 7, 1));
    EXPECT_EQ((*jb)->reanchor_count(), 0u);

    (*jb)->pull(out); // 应用 reanchor；target 尚未填满，因此本次 pull 是恢复阶段的 Hold/静音
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 7u);

    // 把新 target 窗口填到足以脱离 Hold，再从新锚点开始消费。
    ASSERT_TRUE(push_frame(**jb, 8, 1));
    ASSERT_TRUE(push_frame(**jb, 9, 1));

    const auto r = (*jb)->pull(out);
    ASSERT_EQ(r.frames_filled, 1u);
    EXPECT_EQ(r.silence_frames, 0u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(out[0]), 8u);
}

TEST(JitterBufferBoundaryTest, ReanchorRequestUsesFarthestPendingSequence)
{
    auto jb = JitterBuffer::create(make_config(30, 1));
    ASSERT_TRUE(jb.has_value());

    // 先建立播放时间线，再发出多个远超前请求。
    for (std::uint64_t s = 0; s < 18; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out); // play=1，highest=17

    // 两个目标都落在空槽上；第二个请求更远，必须胜出。
    ASSERT_TRUE(push_frame(**jb, 48, 1));
    ASSERT_TRUE(push_frame(**jb, 60, 1));

    (*jb)->pull(out);
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 60u);
}

TEST(JitterBufferBoundaryTest, ReanchorRequestKeepsFarthestPendingSequence)
{
    auto jb = JitterBuffer::create(make_config(30, 1));
    ASSERT_TRUE(jb.has_value());

    // 先填满启动 target 建立播放时间线，再发出多个远超前请求。
    for (std::uint64_t s = 0; s < 18; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out); // play=1，highest=17

    // 两个目标分别映射到已被消费/从未占用的槽；第二个请求更远，
    // 必须在单槽 mailbox 中胜出。
    ASSERT_TRUE(push_frame(**jb, 30, 1));
    ASSERT_TRUE(push_frame(**jb, 48, 1));

    (*jb)->pull(out);
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 48u);
}

TEST(JitterBufferBoundaryTest, ReanchorEscapesSecondGapWhileHold)
{
    auto cfg = make_config(30, 1);
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s < 6; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out); // play=1，highest=5

    // 第一个远超前帧进入正常占用路径，作为 live-edge 观测被保留；
    // 因为缺口已跨越接收窗口，延迟请求应立即被应用。
    ASSERT_TRUE(push_frame(**jb, 1000, 1));
    for (int i = 0; i < 2 && (*jb)->reanchor_count() == 0; ++i) {
        (*jb)->pull(out);
    }
    ASSERT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 1000u);

    // Hold 期间触发帧仍是当前槽；第二个远超前帧落在同一槽会被拒绝，
    // 但它的请求必须存留下来并脱离 Hold。
    ASSERT_TRUE(push_frame(**jb, 1001, 1));
    ASSERT_TRUE(push_frame(**jb, 1002, 1));
    EXPECT_FALSE(push_frame(**jb, 1030, 1));

    for (int i = 0; i < 20 && (*jb)->reanchor_count() < 2; ++i) {
        (*jb)->pull(out);
    }
    EXPECT_EQ((*jb)->reanchor_count(), 2u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 1030u);
}

TEST(JitterBufferBoundaryTest, PreStartFarAheadReanchors)
{
    auto jb = JitterBuffer::create(make_config(30, 1));
    ASSERT_TRUE(jb.has_value());

    ASSERT_TRUE(push_frame(**jb, 0, 1));
    ASSERT_TRUE(push_frame(**jb, 100, 1));

    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out);
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ((*jb)->last_reanchor_sequence(), 100u);
}

TEST(JitterBufferBoundaryTest, RejectsAbsurdReanchorJump)
{
    auto jb = JitterBuffer::create(make_config(30, 1));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t seq = 0; seq < 18; ++seq) {
        ASSERT_TRUE(push_frame(**jb, seq, 1));
    }
    std::vector<std::byte> out(kFrameBytes);
    (*jb)->pull(out);

    EXPECT_FALSE(push_frame(**jb, 1'000'002, 1));
    EXPECT_EQ((*jb)->reanchor_sanity_rejections(), 1u);
    EXPECT_EQ((*jb)->reanchor_count(), 0u);
}

TEST(JitterBufferBoundaryTest, RejectsUnalignedPullOutput)
{
    auto jb = JitterBuffer::create(make_config(4, 1));
    ASSERT_TRUE(jb.has_value());

    std::vector<std::byte> out(5, std::byte { 0x7f });
    const auto r = (*jb)->pull(out);
    EXPECT_EQ(r.frames_filled, 0u);
    EXPECT_EQ(out[4], std::byte { 0x7f });
}

} // namespace
