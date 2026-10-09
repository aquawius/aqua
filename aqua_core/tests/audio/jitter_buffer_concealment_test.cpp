// Phase 2：PCM concealment（repeat-last + 增益表淡出 + 只衰减能量匹配 + 静音封顶）
// 与欠载/迟到计数。
//
// 覆盖细则 §9（concealment 必须有最大连续长度，超过上限进静音）、§14（late 包
// 继续 drop，但记录 usefulness potential）与 §8/§11（underrun budget 计数器）。
//
// 用 F32LE 是为了能直接读回浮点样本验证增益；F 取 4 让一次 pull 正好等于
// 一个 slot，便于逐槽断言。淡出表默认指数 2^(-i/2)（每槽 -3dB；只有偶数索引
// 落回 2 的幂），配合稳态内容（能量匹配落入死区 → 系数精确 1.0）让期望值可按
// 解析式算出来，而不是"实现抄实现"。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/buffer/target_controller.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
//  max_slots 的默认实参 3 是**本文件自己的标定值**（F=4 下"掩盖 3 槽、第 4 槽静音"
//  的几何好算），与组件默认（config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS）无关；
//  默认取值本身由 DefaultWindowConcealsExactlyToConfigConstant 单独钉。
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

// 恒定值真实帧：稳态内容让能量 EMA 引导后逐槽不变（ema == last_ms），能量匹配
// 落入死区 → 系数精确 1.0，断言只受淡出表影响（首包逐字重复快路径也依赖它）。
bool push_const_frame(JitterBuffer& jb, std::uint64_t seq, float value)
{
    const auto data = make_float_payload(value);
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

TEST(JitterBufferConcealmentTest, RepeatsLastPacketWithGainTableFadeThenSilence)
{
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    // 稳态内容（恒定 2.0，几何同 push_with_four_hole_run）：能量匹配死区 →
    // 系数 1.0，本用例只锁定淡出表形状——指数 2^(-i/2) → 增益 1.0 / 2^(-1/2) / 0.5
    // （淡出表不再是全 2 的幂：奇数索引为非幂值，期望值改用 NEAR + 1e-3 容差，
    //  避开"先乘后除 / 先除后乘"的舍入顺序差异；偶数索引仍精确，保留 FLOAT_EQ。）
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 2.0f));
    }
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 2.0f));
    }

    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 0 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 1 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 2 真实（掩盖基准）
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 3 掩盖 #1：增益 1.0，逐字重复
    EXPECT_NEAR(pull_slot(*jb), 2.0f * 23170.0f / 32768.0f, 1e-3f); // slot 4 掩盖 #2：×2^(-1/2) ≈ 1.4142
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 5 掩盖 #3：×0.5（表 i=2 落回 2 的幂）
    // 第 4 个缺帧 slot：超过连续上限 → 静音（细则 §9）。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f); // slot 6
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 7 真实数据恢复
}

TEST(JitterBufferConcealmentTest, EnergyMatchAttenuatesLoudOutlierSlot)
{
    // 缺帧前恰好是响瞬态（鼓点）：context 远轻于被重复包 → 只衰减匹配，
    // 不把事故原样重播（"鬼畜鼓点"防护）。
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    // slot 0..2 安静（1.0），slot 3 响（4.0），4/5/6 留洞，7..21 安静。
    // 几何与 push_with_four_hole_run 同族。lead 按 span 计（highest-play+1，洞也占
    // 跨度），且水位修正在 consumer 侧评估——集中推包后首次评估看到最大 span：
    // 预滚 span=22 < normal_high=24（DROP 起点；>warning_high=27 升级为 deadline-high
    // 大步跳），消费 4 槽后 lead=18 ≥ normal_low=11 不触发 FILL——全程 normal 区，
    // 能量匹配是唯一变量。
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 1.0f));
    }
    ASSERT_TRUE(push_const_frame(*jb, 3, 4.0f));
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 1.0f));
    }
    for (int i = 0; i < 4; ++i) {
        pull_slot(*jb); // 消费 slot0..3（slot3 的响包喂进 EMA）
    }
    // EMA：引导后恒定内容恒 1.0；slot3 后 ema = 1 + (16-1)/8 = 2.875。
    // last_ms = 16 → ratio2 = 0.1796875 < 死区 0.81 → scale = √ratio2，
    // energy_q15 = 13890。逐槽增益 = (表[run] × 13890 + 16384) >> 15：
    // 表值 32768 / 23170 / 16384 → run0=13890 / run1=9821 / run2=6945（含 +0.5 舍入）。
    EXPECT_NEAR(pull_slot(*jb), 4.0f * 13890.0f / 32768.0f, 1e-3f); // slot 4 ≈ 1.6956（表 1.0 折叠）
    EXPECT_NEAR(pull_slot(*jb), 4.0f * 9821.0f / 32768.0f, 1e-3f); // slot 5 ≈ 1.1990（表 2^(-1/2) 折叠）
    EXPECT_NEAR(pull_slot(*jb), 4.0f * 6945.0f / 32768.0f, 1e-3f); // slot 6 ≈ 0.8478（表 0.5 折叠）
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 7 真实恢复
}

TEST(JitterBufferConcealmentTest, EnergyMatchNeverAmplifies)
{
    // 缺帧前是轻过门、context 更响：ratio2 > 死区 → 系数钳制 1.0，逐字重复。
    // 放大轻包 = 放大本底噪声，宁可原样（只衰减契约的另一半）。
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    // slot 0..2 响（4.0），slot 3 轻（1.0），4/5/6 留洞，7..21 响。
    // 几何约束同上一用例（lead 全程 normal 区）。
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 4.0f));
    }
    ASSERT_TRUE(push_const_frame(*jb, 3, 1.0f));
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 4.0f));
    }
    for (int i = 0; i < 4; ++i) {
        pull_slot(*jb); // 消费 slot0..3
    }
    // EMA 恒 16 → slot3 后 ema = 14.125；last_ms = 1 → ratio2 = 14.125 > 1
    // → 只衰减契约钳制系数 1.0。期望值只受淡出表 2^(-i/2) 影响。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 4 掩盖 #1：逐字重复，未放大
    EXPECT_NEAR(pull_slot(*jb), 23170.0f / 32768.0f, 1e-3f); // slot 5：×2^(-1/2) ≈ 0.7071
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.5f); // slot 6：×0.5（表 i=2 落回 2 的幂）
    EXPECT_FLOAT_EQ(pull_slot(*jb), 4.0f); // slot 7 真实恢复
}

TEST(JitterBufferConcealmentTest, NearSilenceLastSlotRepeatsExactly)
{
    // 被重复包近静音（< -60dBFS）：跳过能量匹配（防无意义除法/缩放），逐字重复。
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);
    // slot 0..2 正常（1.0），slot 3 近静音（1e-4，ms=1e-8 < 阈值 1e-6），
    // 4/5 留洞，6..20 恢复。几何约束同上（预滚 lead=21，全程 normal 区）。
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 1.0f));
    }
    ASSERT_TRUE(push_const_frame(*jb, 3, 1e-4f));
    for (std::uint64_t seq = 6; seq <= 20; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 1.0f));
    }
    for (int i = 0; i < 4; ++i) {
        pull_slot(*jb); // 消费 slot0..3
    }
    // 系数 1.0：首包逐字；第二包 ×2^(-1/2)（新曲线下不再是精确减半，
    // 且 1e-4 量级需要专门的容差）。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1e-4f); // slot 4
    EXPECT_NEAR(pull_slot(*jb), 1e-4f * 23170.0f / 32768.0f, 1e-9f); // slot 5：×2^(-1/2)
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f); // slot 6 真实恢复
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

// 一个 NaN 样本不得永久毒化能量 EMA，也不得把非有限值喂给 std::lround。
//
// 旧实现：note_real_audio 直接 `ema += alpha*(ms - ema)`，而 mean_square 里
// `acc += v*v` 遇 NaN → acc=NaN ⇒ ema **永久** NaN（递推再也出不来，只有 reset 清）。
// 之后 update_conceal_energy 的三道守卫 `<=` / `<` / `>=` 对 NaN 全部返回 false，
// 于是 NaN 一路穿到 std::lround —— domain error、返回值未指定，再把未指定值转成
// uint32_t 又是 UB。
//
// 触发前提是 F32 + 非有限样本，而 F32 正是 WASAPI 共享模式的生产格式，所以这不是
// 纯理论。断言只锁"EMA 被复位、后续正常样本重新引导"，不试图断言 UB 本身。
TEST(JitterBufferConcealmentTest, NonFiniteSampleDoesNotPoisonEnergyEma)
{
    auto jb = make_buffer(true);
    ASSERT_NE(jb, nullptr);

    // 预滚到锚定（startup_slots_ = 15）。几何借 push_with_four_hole_run：洞是 3..6。
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_frame(*jb, seq));
    }
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_frame(*jb, seq));
    }

    // 消费掉三个真实 slot（值 1/2/3），play 停在 3，洞 3..6 待掩盖。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 1.0f);
EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);

    // 把 seq 3 填成一个含 NaN 的 slot，并消费掉它。
// 旧实现在这里把 real_ms_ema_ **永久** 变成 NaN（EMA 递推再也出不来）。
    {
        auto data = make_float_payload(2.0f);
        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(data.data(), &nan_value, sizeof nan_value);
        AudioFrame f { 3, kFrames, std::span<const std::byte>(data) };
        ASSERT_TRUE(jb->push(f));
    }
    (void)pull_slot(*jb); // 输出含 NaN —— 这是"逐字重复上一个真实样本"的既定语义

    // 再填入一个正常 slot（恒定 5.0）并消费：这条正常样本必须能把能量 EMA 重新
 // 引导成有限值（修复后这里是"复位 + 重新引导"，而不是继续带着 NaN）。
ASSERT_TRUE(push_const_frame(*jb, 4, 5.0f));
    const auto resumed = pull_slot(*jb);
    EXPECT_TRUE(std::isfinite(resumed)) << "normal slot after a NaN slot must be finite";
    EXPECT_FLOAT_EQ(resumed, 5.0f);

// 最后再留一个洞：掩盖必须按淡出表**成比例**衰减，且全程有限。
 //
    // 这一步判定"能量系数被稳定算出"：系数 = fade(run) × energy_coef，energy_coef
    // 在整个 episode 内只算一次（update_conceal_energy 在 episode 入口调用），所以
    // 相邻两 run 的比值必须精确等于淡出表的 2^(-1/2)。若 real_ms_ema_ 还带着 NaN，
    // update_conceal_energy 会把非有限值一路带到 std::lround（UB：domain error +
    // 未指定值），得到的系数不会再遵循任何规律。
    //
    // 注意这里**不断言**系数等于 1.0：修复后的行为是"跳过非有限样本、保留此前的
    // 有限 EMA"，随后正常样本会按 EMA 时间常数把水位拉向新电平——所以第一次掩盖
    // 就落在死区之外是正常的，不是缺陷。
    //
    // 同样**不**去断言"修复前的具体输出"：旧路径是 UB，可能静音也可能垃圾值，没有
    // 确定性可断言。能锁定的是修复后这条路径给出有限且严格遵循淡出曲线的序列。
    const auto c0 = pull_slot(*jb);
    const auto c1 = pull_slot(*jb);
    EXPECT_TRUE(std::isfinite(c0) && std::isfinite(c1)) << "concealment after recovery must be finite";
    EXPECT_GT(c0, 0.0f) << "concealment must not be muted by a garbage gain";
EXPECT_NEAR(static_cast<double>(c1) / static_cast<double>(c0),
        std::sqrt(0.5), 1e-3)
        << "consecutive concealment must follow the fade curve (2^(-1/2) per run)";
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
    // 复位后能量 EMA 一并清零（未引导 → 系数 1.0）。用稳态内容验证：即便开着
    // concealment，缺帧时也是逐字重复上一个真实包（几何同 four_hole_run）。
    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 3.0f));
    }
    for (std::uint64_t seq = 7; seq <= 21; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 3.0f));
    }
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 3.0f); // slot 3 有基准可重复（系数 1.0，逐字）
}

// 产品默认掩盖窗口的边界（= config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS）必须被钉住：
// 掩盖恰好 window 个缺帧 slot，第 window+1 个起转静音。本文件其余用例一律用测试自己
// 写死的 max_slots（见 make_buffer 的默认实参）标定**语义**，所以"默认取值本身"没有
// 任何断言覆盖——2026-10 上限由 3 抬到 5 时全套用例照旧全绿（语义用例参数化、两个
// replay harness 只做宽松边界断言），这条补上那个缺口。思路与 target_controller_test
// 的 StallPeakCapDefaultsToConfigConstant 一致：语义用例钉参数，默认值单独钉。
TEST(JitterBufferConcealmentTest, DefaultWindowConcealsExactlyToConfigConstant)
{
    const std::uint32_t window = aqua::config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS;
    // 下面的几何按 F=4 的槽位算：真实 0..2、洞 3..(window+4)、其余 window+5..22。
    // 洞数 = window + 2（window 个掩盖 + 2 个封顶静音）；预滚 lead = 23，最后一次
    // 消费时 lead = 23 - (window+5) >= 12，全程落在 normal 区（normal_low=11 /
    // normal_high=24），不混入 Fill/Drop episode。
    ASSERT_GE(window, 4u) << "本用例显式断言前 4 个掩盖槽的增益，默认窗口缩短请同步改这里";
    ASSERT_LE(window, 6u) << "窗口更长时上述几何会掉出 normal 区，需要重新标定槽位";

    // 刻意不设 c.concealment.max_slots → 走组件默认（本用例要验的就是它）。
    JitterBufferConfig c;
    c.capacity_slots = kCapacity;
    c.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, 48000 };
    c.frame_count = kFrames;
    c.concealment.enabled = true;
    auto created = JitterBuffer::create(c);
    ASSERT_TRUE(created.has_value());
    auto jb = std::move(*created);

    for (std::uint64_t seq : { 0ULL, 1ULL, 2ULL }) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 2.0f));
    }
    for (std::uint64_t seq = window + 5; seq <= 22; ++seq) {
        ASSERT_TRUE(push_const_frame(*jb, seq, 2.0f));
    }

    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 0 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 1 真实
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // slot 2 真实（掩盖基准；能量匹配死区 → 系数 1.0）
    // 前 4 个掩盖槽：逐槽查淡出表（表 = round(2^(-i/2) × 32768)，整数化后逐槽可比）。
    EXPECT_NEAR(pull_slot(*jb), 2.0f * 32768.0f / 32768.0f, 1e-3f); // run 0
    EXPECT_NEAR(pull_slot(*jb), 2.0f * 23170.0f / 32768.0f, 1e-3f); // run 1：2^(-1/2)
    EXPECT_NEAR(pull_slot(*jb), 2.0f * 16384.0f / 32768.0f, 1e-3f); // run 2：0.5
    EXPECT_NEAR(pull_slot(*jb), 2.0f * 11585.0f / 32768.0f, 1e-3f); // run 3：2^(-3/2)
    // 一直到 window 个为止都还在掩盖（尚未封顶）。
    for (std::uint32_t run = 4u; run < window; ++run) {
        EXPECT_GT(pull_slot(*jb), 0.0f) << "run " << run << " 应仍在掩盖（未到默认上限）";
    }
    // 第 window+1 / window+2 个缺帧槽：封顶 → 静音。
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 0.0f);
    EXPECT_FLOAT_EQ(pull_slot(*jb), 2.0f); // 真实数据恢复

    EXPECT_EQ(jb->concealed_slots(), static_cast<std::uint64_t>(window));
    EXPECT_EQ(jb->concealed_saturated_slots(), 2u);
    EXPECT_EQ(jb->max_consecutive_underrun_slots(), static_cast<std::uint64_t>(window) + 2u);
    EXPECT_EQ(jb->underrun_events(), 1u); // 连续缺帧只算 1 次事件
    EXPECT_EQ(jb->underrun_frames(), static_cast<std::uint64_t>(window + 2u) * kFrames);
}

} // namespace
