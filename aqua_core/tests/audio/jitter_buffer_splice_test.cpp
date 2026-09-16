#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/buffer_config.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

// S16LE 单声道 → 每采样帧 2 字节。整数域 blend 公式可手算精确期望值，
// 避免 float 舍入让断言变成"实现抄实现"。
constexpr std::uint32_t kFrameBytes = 2;
constexpr std::uint32_t kSlots = 10;
constexpr std::uint32_t kSlotFrames = 4;

aqua::audio::AudioFormat make_format()
{
    return aqua::audio::AudioFormat { aqua::audio::AudioEncoding::PCM_S16LE, 1, 48000 };
}

aqua::audio::JitterBufferConfig make_splice_config()
{
    aqua::audio::JitterBufferConfig c;
    c.capacity_slots = kSlots;
    c.format = make_format();
    c.frame_count = kSlotFrames;
    c.splice.enabled = true;
    c.splice.xfade_frames = 4; // 小 crossfade，手算可验证；构造期钳制下限为 2
    return c;
}

// slot 内第 i 个采样 = seq * 16 + i * 8（S16LE）。相邻包内容差异大，
// 混合是否发生一目了然；数值经 hand-check 落在 int16 范围内。
// blend 公式：out = prev + ((cur - prev) * w + denom/2) / denom，denom = X - 1。
std::vector<std::byte> make_payload(std::uint64_t seq)
{
    std::vector<std::byte> data(static_cast<std::size_t>(kSlotFrames) * kFrameBytes);
    for (std::uint32_t i = 0; i < kSlotFrames; ++i) {
        const auto v = static_cast<std::int16_t>(seq * 16 + i * 8);
        data[static_cast<std::size_t>(i) * kFrameBytes] = static_cast<std::byte>(v & 0xFF);
        data[static_cast<std::size_t>(i) * kFrameBytes + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
    }
    return data;
}

bool push_frame(aqua::audio::JitterBuffer& jb, std::uint64_t seq)
{
    auto data = make_payload(seq);
    aqua::audio::AudioFrame f { seq, kSlotFrames, std::span<const std::byte>(data) };
    return jb.push(f);
}

std::int16_t sample_at(const std::vector<std::byte>& out, std::uint32_t frame_idx)
{
    std::int16_t v = 0;
    const auto* p = out.data() + static_cast<std::size_t>(frame_idx) * kFrameBytes;
    v = static_cast<std::int16_t>(static_cast<std::uint8_t>(p[0])
        | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8));
    return v;
}

std::vector<std::byte> pull_n(aqua::audio::JitterBuffer& jb, std::uint32_t frames)
{
    std::vector<std::byte> out(static_cast<std::size_t>(frames) * kFrameBytes);
    const auto r = jb.pull(out);
    EXPECT_EQ(r.frames_filled, frames);
    return out;
}

// capacity=10/F=4/默认带：target=6 normal=[4,8] warning=[2,9] startup=5。
// push 0..5 → 锚定 play=0；消费 0,1,2 后 lead=3 落进 FILL 区。
// 返回时 JB 处于 FILL episode 进行中（play=3，重播预算已立）。
std::unique_ptr<aqua::audio::JitterBuffer> drive_to_fill()
{
    auto jb = aqua::audio::JitterBuffer::create(make_splice_config());
    EXPECT_TRUE(jb.has_value());
    for (std::uint64_t s = 0; s < 6; ++s) {
        EXPECT_TRUE(push_frame(**jb, s));
    }
    pull_n(**jb, 4); // slot0
    pull_n(**jb, 4); // slot1
    pull_n(**jb, 4); // slot2，play=3
    return std::move(*jb);
}

TEST(JitterBufferSpliceTest, DefaultsAreDocumentedConstants)
{
    EXPECT_EQ(aqua::config::JB_SPLICE_DEFAULT_XFADE_FRAMES, 64u);
    aqua::audio::JitterBufferConfig c;
    EXPECT_FALSE(c.splice.enabled); // 组件默认关 = v1 硬拼接，老测试行为不变
}

TEST(JitterBufferSpliceTest, FillReplayHeadBlendsFromLastPlayedSample)
{
    auto jb = drive_to_fill();
    // lead=3 < normal_low=4 → FILL enter；k=8 跨过整槽触发重播发射。
    // 混合基准 = 最后一个已播采样（slot3 尾 = 72），不是此前 X 帧的头部：
    // w=0 → 72 精确（与已播值连续）；w=1 → 72+(-16+1)/3=67；
    // w=2 → 72+(-32+1)/3=67（截断精确）；w=3 → 72 精确。
    // 纯重播应为 [48,56,64,72]。
    auto out = pull_n(*jb, 8);
    EXPECT_EQ(sample_at(out, 0), 48);
    EXPECT_EQ(sample_at(out, 1), 56);
    EXPECT_EQ(sample_at(out, 2), 64);
    EXPECT_EQ(sample_at(out, 3), 72);
    EXPECT_EQ(sample_at(out, 4), 72);
    EXPECT_EQ(sample_at(out, 5), 67);
    EXPECT_EQ(sample_at(out, 6), 67);
    EXPECT_EQ(sample_at(out, 7), 72);
    EXPECT_EQ(jb->fill_corrected_slots(), 1u);
}

TEST(JitterBufferSpliceTest, DropLandingHeadBlendsFromLastPlayedSample)
{
    auto jb = drive_to_fill();
    // P4 (k=4) 先输出 slot3 原遍 + 立重播预算；再推 6,7,8 让 lead 回 6，
    // P5 完成 FILL（输出 slot3，重播预算在 complete 中蒸发），play=4。
    pull_n(*jb, 4);
    EXPECT_TRUE(push_frame(*jb, 6));
    EXPECT_TRUE(push_frame(*jb, 7));
    EXPECT_TRUE(push_frame(*jb, 8));
    pull_n(*jb, 4); // FILL complete
    // burst 到 deadline 之上（P6 lead=13-4+1=10 > warning_high=9）。
    // 注意 14 会撞上尚未推进的 slot4 槽位被 busy 拒绝，只推到 13。
    // deadline skip = min(10-6,10) = 4：play 4→8，着陆 slot8=[128,136,144,152]，
    // 混合基准 = P5 输出尾（slot3 尾采样 72）：
    // w=0 → 72；w=1 → 72+(64+1)/3=93；w=2 → 72+(144+1)/3=120；
    // w=3 → 72+(240+1)/3=152 精确。
    for (std::uint64_t s = 9; s <= 13; ++s) {
        ASSERT_TRUE(push_frame(*jb, s));
    }
    std::vector<std::byte> out(4 * kFrameBytes);
    const auto r = jb->pull(out);
    EXPECT_EQ(r.frames_filled, 4u);
    EXPECT_GT(r.skipped_slots, 0u);
    EXPECT_EQ(sample_at(out, 0), 72);
    EXPECT_EQ(sample_at(out, 1), 93);
    EXPECT_EQ(sample_at(out, 2), 120);
    EXPECT_EQ(sample_at(out, 3), 152);
}

TEST(JitterBufferSpliceTest, SteadyRegionsStayBitExact)
{
    auto jb = drive_to_fill();
    // 无 arm 触发的 pull 输出必须逐字精确（crossfade 只发生在拼接点）。
    auto out = pull_n(*jb, 4); // slot3 原遍（FILL enter 的 pull）
    EXPECT_EQ(sample_at(out, 0), 48);
    EXPECT_EQ(sample_at(out, 3), 72);
}

TEST(JitterBufferSpliceTest, SilenceResumeFadesInFromSilence)
{
    auto jb = aqua::audio::JitterBuffer::create(make_splice_config());
    ASSERT_TRUE(jb.has_value());
    // 先空拉一包静音（pre-roll 未锚定），再推包锚定：首包真实音频从静音淡入。
    // S16 静音 = 0。slot0 = [0,8,16,24]：w=0 → 0 精确；
    // w=1 → 0+(8+1)/3=3；w=2 → 0+(32+1)/3=11；w=3 → 0+(72+1)/3=24 精确。
    pull_n(**jb, 4);
    for (std::uint64_t s = 0; s < 6; ++s) {
        EXPECT_TRUE(push_frame(**jb, s));
    }
    auto out = pull_n(**jb, 4);
    EXPECT_EQ(sample_at(out, 0), 0);
    EXPECT_EQ(sample_at(out, 1), 3);
    EXPECT_EQ(sample_at(out, 2), 11);
    EXPECT_EQ(sample_at(out, 3), 24);
}

TEST(JitterBufferSpliceTest, ConcealExitBlendsFromLastConcealedSample)
{
    auto cfg = make_splice_config();
    cfg.concealment.enabled = true;
    cfg.concealment.max_slots = 3;
    auto jb = aqua::audio::JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());
    // 缺 seq=6 一包：P1~P6 消费 slot0..5（lead 始终健康，无 FILL/DROP 干扰），
    // P7 掩盖 slot6（首包增益 1.0，精确重复 slot5），P8 slot7 就绪即恢复沿。
    // slot5 = [80,88,96,104]，slot7 = [112,120,128,136]：
    // w=0 → 104 精确；w=1 → 104+(16+1)/3=109；w=2 → 104+(48+1)/3=120；
    // w=3 → 104+(96+1)/3=136 精确。
    for (std::uint64_t s = 0; s <= 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s));
    }
    pull_n(**jb, 4); // 锚定 + slot0
    pull_n(**jb, 4); // slot1
    EXPECT_TRUE(push_frame(**jb, 7));
    EXPECT_TRUE(push_frame(**jb, 8));
    pull_n(**jb, 4); // slot2
    pull_n(**jb, 4); // slot3
    pull_n(**jb, 4); // slot4
    auto slot5 = pull_n(**jb, 4); // slot5 = [80,88,96,104]，掩盖源与混合基准
    EXPECT_EQ(sample_at(slot5, 3), 104);
    // P7：lead=8-6+1=3 < normal_low=4 → FILL enter，但 slot6 缺失走掩盖分支。
    // 掩盖起始同样 arm（此前输出尾 = slot5 尾 104），输出是混合后的
    // [104,99,99,104] 而非纯 slot5——这正是 conceal onset crossfade。
    auto missing = pull_n(**jb, 4);
    EXPECT_EQ(sample_at(missing, 0), 104);
    EXPECT_EQ(sample_at(missing, 1), 99);
    EXPECT_EQ(sample_at(missing, 2), 99);
    EXPECT_EQ(sample_at(missing, 3), 104);
    // P8：slot7 就绪，conceal→真实恢复沿。
    auto out = pull_n(**jb, 4);
    EXPECT_EQ(sample_at(out, 0), 104);
    EXPECT_EQ(sample_at(out, 1), 109);
    EXPECT_EQ(sample_at(out, 2), 120);
    EXPECT_EQ(sample_at(out, 3), 136);
}

TEST(JitterBufferSpliceTest, ReanchorHoldSilenceFadesFromPreviousTail)
{
    auto jb = aqua::audio::JitterBuffer::create(make_splice_config());
    ASSERT_TRUE(jb.has_value());
    for (std::uint64_t s = 0; s <= 5; ++s) {
        EXPECT_TRUE(push_frame(**jb, s));
    }
    pull_n(**jb, 4); // 锚定 + slot0
    pull_n(**jb, 4); // slot1 = [16,24,32,40]，尾采样 40
    // 远超前触发帧落空槽（idx6 空）：sanity 通过，oldest 前移 + reanchor 请求。
    // P3：启动后路径 lead=30-2+1=29 ≥ capacity=10 → 应用，play=30；
    // 落后 lead=1 < target → Hold 静音（reanchor 后必然先静音等水位）。
    // 静音起始从此前输出尾（40）淡出：w=0 → 40；w=1 → 40+(0-40+1)/3=27；
    // w=2 → 40+(0-80+1)/3=14；w=3 → 40+(0-120+1)/3=1。
    EXPECT_TRUE(push_frame(**jb, 30));
    auto out = pull_n(**jb, 4);
    EXPECT_EQ((*jb)->reanchor_count(), 1u);
    EXPECT_EQ(sample_at(out, 0), 40);
    EXPECT_EQ(sample_at(out, 1), 27);
    EXPECT_EQ(sample_at(out, 2), 14);
    EXPECT_EQ(sample_at(out, 3), 1);
}

} // namespace
