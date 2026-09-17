// Phase 2：PCM concealment（repeat-last + 线性淡出 + 静音封顶）与欠载/迟到计数。
//
// 覆盖细则 §9（concealment 必须有最大连续长度，超过上限进静音）、§14（late 包
// 继续 drop，但记录 usefulness potential）与 §8/§11（underrun budget 计数器）。
//
// 用 F32LE 是为了能直接读回浮点样本验证淡出增益；F 取 4 让一次 pull 正好等于
// 一个 slot，便于逐槽断言。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/buffer/target_controller.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;

constexpr std::uint32_t kFrames = 4; // 每 slot 的 sample frame 数
constexpr std::uint32_t kFrameBytes = 4; // F32LE 单声道
constexpr std::uint32_t kCapacity = 30; // 水位带够宽，测试期间不触发 Fill/Drop

std::vector<std::byte> make_float_payload(float value)
{
    std::vector<std::byte> data(static_cast<std::size_t>(kFrames) * kFrameBytes);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        std::memcpy(data.data() + static_cast<std::size_t>(i) * kFrameBytes, &value, sizeof value);
    }
    return data;
}

//  concealment_on = false 即 v1 行为（缺帧直接静音）。
std::unique_ptr<JitterBuffer> make_buffer(bool concealment_on, std::uint32_t max_slots = 3)
{
    JitterBufferConfig c;
    c.capacity_slots = kCapacity;
    c.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
    c.frame_count = kFrames;
    c.concealment.enabled = concealment_on;
    c.concealment.max_slots = max_slots;
    auto created = JitterBuffer::create(c);
    return created ? std::move(*created) : nullptr;
}

// 帧值 = seq + 1，便于把"真实数据 / 掩盖数据 / 静音"区分开。
bool push_frame(JitterBuffer& jb, std::uint64_t seq)
{
    const auto data = make_float_payload(static_cast<float>(seq + 1));
    AudioFrame f { seq, kFrames, std::span<const std::byte>(data) };
    return jb.push(f);
}

// 一次 pull 正好一个 slot，返回该 slot 首帧的浮点值。
float pull_slot(JitterBuffer& jb)
{
    std::vector<std::byte> out(static_cast<std::size_t>(kFrames) * kFrameBytes);
    jb.pull(out);
    float value = 0.0f;
    std::memcpy(&value, out.data(), sizeof value);
    return value;
}

// 序列 3/4/5/6 留空（4 个连续洞），其余 0..2 与 7..21 全部入槽。
// 预滚后 lead = 22，逐槽消费到 slot 7 时 lead = 14，全程落在 normal 区
// （normal_low=11 / normal_high=24），不会混入 Fill/Drop episode。
void push_with_four_hole_run(JitterBuffer& jb)
{
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
}

TEST(JitterBufferConcealmentTest, DefaultConfigKeepsLegacySilence)
{
    // 组件默认关：不显式打开时缺帧必须是静音（v1 行为，现有 JB 测试依赖）。
    JitterBufferConfig c;
    c.capacity_slots = kCapacity;
    c.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
    c.frame_count = kFrames;
    auto created = JitterBuffer::create(c);
    ASSERT_TRUE(created.has_value());
    auto jb = std::move(*created);
    push_with_four_hole_run(*jb);

    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 0
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 1
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f); // slot 2
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f); // slot 3 缺帧 → 静音
    EXPECT_EQ(jb->concealed_slots(), 0u);
    EXPECT_EQ(jb->underrun_frames(), 4u);
}

TEST(JitterBufferConcealmentTest, RepeatsLastPacketWithLinearFadeThenSilence)
{
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    push_with_four_hole_run(*jb);

    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 0 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 1 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f); // slot 2 真实（掩盖基准）
    // 掩盖：重复 slot 2 的 PCM，增益依次 1.0 / 2/3 / 1/3（Q15 定点，容差 1e-3）。
    EXPECT_NEAR(pull_slot(*jb), 3.0f, 1e-3f); // slot 3
    EXPECT_NEAR(pull_slot(*jb), 2.0f, 1e-3f); // slot 4
    EXPECT_NEAR(pull_slot(*jb), 1.0f, 1e-3f); // slot 5
    // 第 4 个缺帧 slot：超过连续上限 → 静音（细则 §9）。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f); // slot 6
    EXPECT_FLOAT_EQ(pull_slot(*jb), 8.0f); // slot 7 真实数据恢复
}

TEST(JitterBufferConcealmentTest, ConcealmentCountersSplitFromSilence)
{
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    push_with_four_hole_run(*jb);
    for (int i = 0; i < 8; ++i) {
        pull_slot(*jb);
    }

    EXPECT_EQ(jb->concealed_slots(), 3u); // 掩盖 3 个 slot
    EXPECT_EQ(jb->concealed_saturated_slots(), 1u); // 第 4 个封顶转静音
    EXPECT_EQ(jb->underrun_events(), 1u); // 4 个连续缺帧合并成一次
    EXPECT_EQ(jb->max_consecutive_underrun_slots(), 4u);
    // underrun = 掩盖帧 + 缺帧静音帧 = 4 slot × 4 帧
    EXPECT_EQ(jb->underrun_frames(), 16u);
    // 掩盖帧不是静音：silence 只统计真正输出静音的那一个 slot。
    EXPECT_EQ(jb->pull_silence_frames(), 4u);
    EXPECT_EQ(jb->pull_frames(), 32u);
}

TEST(JitterBufferConcealmentTest, DisabledConcealmentSilencesEveryMissingSlot)
{
    auto jb = make_buffer(false);
    ASSERT_NE(jb, nullptr);
    push_with_four_hole_run(*jb);
    for (int i = 0; i < 8; ++i) {
        pull_slot(*jb);
    }

    EXPECT_EQ(jb->concealed_slots(), 0u);
    EXPECT_EQ(jb->concealed_saturated_slots(), 0u);
    EXPECT_EQ(jb->underrun_events(), 1u);
    EXPECT_EQ(jb->max_consecutive_underrun_slots(), 4u);
    EXPECT_EQ(jb->underrun_frames(), 16u);
    EXPECT_EQ(jb->pull_silence_frames(), 16u); // 全部走静音
}

TEST(JitterBufferConcealmentTest, ZeroMaxSlotsDisablesConcealment)
{
    auto jb = make_buffer(true, 0);
    ASSERT_NE(jb, nullptr);
    push_with_four_hole_run(*jb);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f); // max_slots=0 → 关闭，静音
    EXPECT_EQ(jb->concealed_slots(), 0u);
}

TEST(JitterBufferConcealmentTest, LatePacketsWithinConcealWindowAreCountedAsUseful)
{
    // 细则 §14：late 包继续 drop，但记录"本可用"潜力（落后 <= max_slots）。
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    auto& buf = *jb;
    for (std::uint64_t seq = 0; seq <= 21; ++seq) {
        ASSERT_TRUE(push_frame(buf, seq));
    }
    // 消费到 play=9（lead 13，仍在 normal 区）。
    for (int i = 0; i < 9; ++i) {
        pull_slot(buf);
    }
    ASSERT_EQ(buf.play_sequence(), 9u);
    ASSERT_EQ(buf.late_useful_packets(), 0u);

    EXPECT_FALSE(push_frame(buf, 8)); // 落后 1 slot → 可用
    EXPECT_FALSE(push_frame(buf, 7)); // 落后 2 slot → 可用
    EXPECT_FALSE(push_frame(buf, 6)); // 落后 3 slot → 可用（= 窗口）
    EXPECT_FALSE(push_frame(buf, 4)); // 落后 5 slot → 太晚，不计数
    EXPECT_EQ(buf.late_useful_packets(), 3u);
    EXPECT_GE(buf.push_rejected_late(), 4u);
}

// 2 槽洞（全被掩盖）与 5 槽洞（3 掩盖 + 2 溢出）：序列排布与
// push_with_four_hole_run 同思路——预滚 lead 保证全程 normal 区，不混入 Fill/Drop。
void push_with_two_hole_run(JitterBuffer& jb)
{
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
    for (std::uint64_t seq = 5; seq <= 19; ++seq) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
}

void push_with_five_hole_run(JitterBuffer& jb)
{
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
    for (std::uint64_t seq = 8; seq <= 22; ++seq) {
        ASSERT_TRUE(push_frame(jb, seq));
    }
}

TEST(JitterBufferConcealmentTest, PenaltyMappingToleratesCoveredGaps)
{
    using aqua::audio::select_penalty_events;
    // 2 槽洞：欠载确实发生了（1 次事件），但全被掩盖 → 计罚 0（低延迟取舍）。
    {
        auto jb = make_buffer(true);
        ASSERT_NE(jb, nullptr);
        push_with_two_hole_run(*jb);
        for (int i = 0; i < 6; ++i) {
            pull_slot(*jb);
        }
        ASSERT_EQ(jb->underrun_events(), 1u);
        EXPECT_EQ(jb->concealed_slots(), 2u);
        EXPECT_EQ(jb->concealed_saturated_slots(), 0u);
        EXPECT_EQ(select_penalty_events(true, jb->underrun_events(),
                      jb->concealed_saturated_slots()),
            0u);
    }
    // 5 槽洞：3 掩盖 + 2 溢出 → 计罚 2（可闻缺损才抬地板）。
    {
        auto jb = make_buffer(true);
        ASSERT_NE(jb, nullptr);
        push_with_five_hole_run(*jb);
        for (int i = 0; i < 9; ++i) {
            pull_slot(*jb);
        }
        ASSERT_EQ(jb->underrun_events(), 1u);
        EXPECT_EQ(jb->concealed_slots(), 3u);
        EXPECT_EQ(jb->concealed_saturated_slots(), 2u);
        EXPECT_EQ(select_penalty_events(true, jb->underrun_events(),
                      jb->concealed_saturated_slots()),
            2u);
    }
    // conceal 关：同样的 2 槽洞每个都是静音 → 计罚退回事件数。
    {
        auto jb = make_buffer(false);
        ASSERT_NE(jb, nullptr);
        push_with_two_hole_run(*jb);
        for (int i = 0; i < 6; ++i) {
            pull_slot(*jb);
        }
        ASSERT_EQ(jb->underrun_events(), 1u);
        EXPECT_EQ(select_penalty_events(false, jb->underrun_events(),
                      jb->concealed_saturated_slots()),
            1u);
    }
}

TEST(JitterBufferConcealmentTest, ResetClearsConcealmentAndUnderrunState)
{
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    push_with_four_hole_run(*jb);
    for (int i = 0; i < 8; ++i) {
        pull_slot(*jb);
    }
    ASSERT_GT(jb->concealed_slots(), 0u);

    jb->reset();
    EXPECT_EQ(jb->concealed_slots(), 0u);
    EXPECT_EQ(jb->concealed_saturated_slots(), 0u);
    EXPECT_EQ(jb->underrun_events(), 0u);
    EXPECT_EQ(jb->underrun_frames(), 0u);
    EXPECT_EQ(jb->max_consecutive_underrun_slots(), 0u);
    EXPECT_EQ(jb->late_useful_packets(), 0u);
    // 复位后没有真实 PCM 可重复：即便开着 concealment 也必须静音。
    push_with_four_hole_run(*jb);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);
    EXPECT_NEAR(pull_slot(*jb), 3.0f, 1e-3f); // slot 3 有基准可重复
}

} // namespace
