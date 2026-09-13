// JitterBuffer 不变量单测：锁定水位/lead 的裁剪契约。
//
// buffer_design.md 修正后的口径：water_level 与 lead_slots 都被裁剪——前者裁剪到
// [0,1]，后者裁剪到 capacity。之所以必须裁剪：reanchor 请求待应用时 highest 会被
// 临时抬高（超窗），若不裁剪会给出 >1 的伪水位或 >capacity 的伪 lead，干扰诊断与
// 上层百分比解释。本文件用对抗性序列（乱序 / 重复 / 迟到 / 巨跳 reanchor / 反复
// 灌满抽空）持续校验这两条不变量，确保任何路径下水位与 lead 都落在合法区间。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;

// PCM_F32LE 单声道 → 每采样帧 4 字节。
constexpr std::uint32_t kFrameBytes = 4;
constexpr std::uint32_t kFrameCount = 4; // 每 AudioFrame 的采样帧数（F）

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

std::vector<std::byte> make_payload(std::uint32_t frame_count, std::uint8_t fill)
{
    std::vector<std::byte> data(static_cast<std::size_t>(frame_count) * kFrameBytes);
    std::fill(data.begin(), data.end(), static_cast<std::byte>(fill));
    return data;
}

bool push_frame(JitterBuffer& jb, std::uint64_t seq)
{
    auto data = make_payload(kFrameCount, static_cast<std::uint8_t>((seq + 1) & 0xFF));
    AudioFrame f { seq, kFrameCount, std::span<const std::byte>(data) };
    return jb.push(f);
}

// 核心不变量：水位 ∈ [0,1]，lead ≤ 容量。任何 push/pull 之后都必须成立。
void assert_invariants(JitterBuffer& jb)
{
    const double w = jb.water_level();
    EXPECT_GE(w, 0.0);
    EXPECT_LE(w, 1.0);
    EXPECT_LE(jb.lead_slots(), jb.capacity_slots());
}

// 抽一个槽（推进 play），随后校验不变量。
void pull_one(JitterBuffer& jb)
{
    std::vector<std::byte> out(static_cast<std::size_t>(kFrameCount) * kFrameBytes);
    jb.pull(out);
    assert_invariants(jb);
}

TEST(JitterBufferInvariantsTest, EmptyBufferHasZeroWaterLevel)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.0);
    EXPECT_EQ((*jb)->lead_slots(), 0u);
    assert_invariants(**jb);
}

TEST(JitterBufferInvariantsTest, FullBufferWaterLevelEqualsOneAndLeadEqualsCapacity)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());
    // 连续灌满全部 16 槽（seq 0..15）。
    for (std::uint64_t s = 0; s < 16; ++s) {
        ASSERT_TRUE(push_frame(**jb, s));
        assert_invariants(**jb);
    }
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 1.0);
    EXPECT_EQ((*jb)->lead_slots(), 16u);
    EXPECT_EQ((*jb)->used_slots(), 16u);
}

TEST(JitterBufferInvariantsTest, AdversarialOutOfOrderSequenceKeepsInvariants)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());

    // 确定性 LCG（可复现），生成 [0,200) 内的乱序/重复序列逐个 push，
    // 每步校验不变量；每隔几步抽一个槽推进 play。
    std::uint64_t state = 0x1234'5678ULL;
    auto next = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    for (int i = 0; i < 500; ++i) {
        const std::uint64_t seq = next() % 200;
        push_frame(**jb, seq); // 返回 true/false 都允许（重复/迟到会被拒），不变量必须保持
        assert_invariants(**jb);
        if (i % 7 == 0) {
            pull_one(**jb);
        }
    }
}

TEST(JitterBufferInvariantsTest, DuplicateAndLateFramesDoNotBreakInvariants)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());
    for (std::uint64_t s = 0; s < 10; ++s) {
        ASSERT_TRUE(push_frame(**jb, s));
    }
    // 重复推同一批 seq（slot 冲突/重复 → 应被拒），不变量不变。
    for (std::uint64_t s = 0; s < 10; ++s) {
        push_frame(**jb, s);
        assert_invariants(**jb);
    }
    // 推进 play（抽若干槽），再推迟到帧（seq < play → 应被拒），不变量保持。
    for (int i = 0; i < 4; ++i) {
        pull_one(**jb);
    }
    const auto play = (*jb)->play_sequence();
    ASSERT_GT(play, 0u);
    for (std::uint64_t s = 0; s < play; ++s) {
        push_frame(**jb, s);
        assert_invariants(**jb);
    }
}

TEST(JitterBufferInvariantsTest, ReanchorFarFutureDoesNotOverflowWaterOrLead)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());
    // 先建立基线（灌满并锚定 play），再推一个远超 highest 的 seq 触发 reanchor。
    for (std::uint64_t s = 0; s < 16; ++s) {
        ASSERT_TRUE(push_frame(**jb, s));
    }
    for (int i = 0; i < 4; ++i) {
        pull_one(**jb); // 锚定 play，使后续远超前判定走 started 分支
    }
    const auto highest_before = (*jb)->highest_received_sequence();
    // 远超 highest（≥ capacity 且 ≥ MIN_GAP）→ 受理为 reanchor 请求，highest 被抬高。
    const std::uint64_t far = highest_before + 100;
    EXPECT_TRUE(push_frame(**jb, far));
    // 关键：即便 highest 被瞬间抬到 far，lead/water 仍被裁剪在合法区间——不变量成立。
    assert_invariants(**jb);
    EXPECT_LE((*jb)->lead_slots(), 16u);
    EXPECT_LE((*jb)->water_level(), 1.0);
    // 抽若干槽让 reanchor 有机会应用；无论是否应用，不变量持续成立。
    for (int i = 0; i < 8; ++i) {
        pull_one(**jb);
    }
}

TEST(JitterBufferInvariantsTest, RepeatedFillDrainCyclesKeepInvariants)
{
    auto jb = JitterBuffer::create(make_config(16, kFrameCount));
    ASSERT_TRUE(jb.has_value());
    std::uint64_t seq = 0;
    for (int cycle = 0; cycle < 20; ++cycle) {
        // 灌满。
        for (std::uint32_t s = 0; s < 16; ++s) {
            ASSERT_TRUE(push_frame(**jb, seq++));
            assert_invariants(**jb);
        }
        // 抽空。
        while ((*jb)->used_slots() > 0) {
            pull_one(**jb);
        }
        EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.0);
    }
}

} // namespace
