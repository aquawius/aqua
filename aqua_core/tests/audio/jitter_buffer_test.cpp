#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"

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
using aqua::audio::JitterBufferPullResult;

// PCM_F32LE 单声道 → 每采样帧 4 字节。
constexpr std::uint32_t kFrameBytes = 4;

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

// 帧内容用 (seq + 1) 填充，便于把"真实数据"与"静音(0)"区分开。
bool push_frame(JitterBuffer& jb, std::uint64_t seq, std::uint32_t frame_count)
{
    auto data = make_payload(frame_count, static_cast<std::uint8_t>((seq + 1) & 0xFF));
    AudioFrame f { seq, frame_count, std::span<const std::byte>(data) };
    return jb.push(f);
}

// 把若干次 pull 的输出拼成一维字节流，便于按 frame 下标断言内容。
std::vector<std::byte> drain(JitterBuffer& jb, std::uint32_t k, std::uint32_t max_pulls,
    JitterBufferPullResult* last = nullptr)
{
    std::vector<std::byte> all;
    for (std::uint32_t i = 0; i < max_pulls; ++i) {
        std::vector<std::byte> out(static_cast<std::size_t>(k) * kFrameBytes);
        const auto r = jb.pull(out);
        all.insert(all.end(), out.begin(), out.end());
        if (last != nullptr) {
            *last = r;
        }
        if (r.frames_filled == 0) {
            break;
        }
    }
    return all;
}

// 返回字节流中第 frame_idx 个采样帧的首字节（作为"填充值"指纹）。
std::uint8_t frame_fill(const std::vector<std::byte>& bytes, std::uint32_t frame_idx)
{
    return static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(frame_idx) * kFrameBytes]);
}

TEST(JitterBufferTest, CreateRejectsInvalidConfig)
{
    // capacity_slots == 0
    EXPECT_FALSE(JitterBuffer::create(make_config(0, 4)).has_value());
    // frame_count == 0
    EXPECT_FALSE(JitterBuffer::create(make_config(10, 0)).has_value());
    // format 非法
    {
        auto c = make_config(10, 4);
        c.format = AudioFormat { };
        EXPECT_FALSE(JitterBuffer::create(c).has_value());
    }
    // 阈值序非法（target 不在 normal_low 与 normal_high 之间）
    {
        auto c = make_config(10, 4);
        c.target = 0.90; // > normal_high
        EXPECT_FALSE(JitterBuffer::create(c).has_value());
    }
}

TEST(JitterBufferTest, CreateReportsCapacities)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());
    EXPECT_EQ((*jb)->capacity_slots(), 10u);
    EXPECT_EQ((*jb)->capacity_bytes(), 10u * 4u * kFrameBytes);
    EXPECT_EQ((*jb)->used_slots(), 0u);
    EXPECT_EQ((*jb)->used_bytes(), 0u);
}

TEST(JitterBufferTest, StartupSilencesUntilLeadReachesTarget)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());

    // target 60% → target_slots = 6。先推 5 帧（lead=5 < 6）。
    for (std::uint64_t s = 0; s < 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    JitterBufferPullResult r { };
    auto out = drain(**jb, 4, 1, &r);
    EXPECT_EQ(r.frames_filled, 4u);
    EXPECT_EQ(r.silence_frames, 4u); // 未达 60%，全静音
    EXPECT_EQ((*jb)->used_slots(), 5u);

    // 再推 1 帧，lead=6，达到目标 → 下一次 pull 建立 anchor 并开始播放。
    ASSERT_TRUE(push_frame(**jb, 5, 4));
    out = drain(**jb, 4, 1, &r);
    EXPECT_EQ(r.frames_filled, 4u);
    EXPECT_EQ(r.silence_frames, 0u);
    EXPECT_EQ(frame_fill(out, 0), 1u); // 首帧 seq=0 → fill=1
}

TEST(JitterBufferTest, AnchorAtOldestNonZeroSequence)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());

    // 首帧 seq 非 0，且乱序到达：102 先到，100/101 后到。
    ASSERT_TRUE(push_frame(**jb, 102, 4));
    ASSERT_TRUE(push_frame(**jb, 101, 4));
    ASSERT_TRUE(push_frame(**jb, 100, 4));
    ASSERT_TRUE(push_frame(**jb, 103, 4));
    ASSERT_TRUE(push_frame(**jb, 104, 4));
    ASSERT_TRUE(push_frame(**jb, 105, 4)); // lead=6

    JitterBufferPullResult r { };
    auto out = drain(**jb, 4, 1, &r);
    EXPECT_EQ(r.silence_frames, 0u);
    // 锚定在最小 seq=100，输出 fill=101（100+1）。
    EXPECT_EQ(frame_fill(out, 0), 101u);
}

TEST(JitterBufferTest, ReorderWithinWindowPreservesOrder)
{
    // N=30, F=4, target 60% → 18 槽。推 18 帧，其中 101/102 乱序到达（102 先于 101）。
    auto jb = JitterBuffer::create(make_config(30, 4));
    ASSERT_TRUE(jb.has_value());

    ASSERT_TRUE(push_frame(**jb, 100, 4));
    ASSERT_TRUE(push_frame(**jb, 102, 4)); // 乱序：102 先到
    ASSERT_TRUE(push_frame(**jb, 101, 4)); // 101 后到
    for (std::uint64_t s = 103; s <= 117; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    // 建立 anchor 并播放首帧（seq 100）。
    drain(**jb, 4, 1);

    // 之后应依次输出 101（fill=102）、102（fill=103）、103（fill=104），
    // 即乱序到达的 101/102 仍按 sequence 顺序播放。
    const auto bytes = drain(**jb, 4, 3);
    EXPECT_EQ(frame_fill(bytes, 0), 102u);
    EXPECT_EQ(frame_fill(bytes, 4), 103u);
    EXPECT_EQ(frame_fill(bytes, 8), 104u);
}

TEST(JitterBufferTest, MissingFrameProducesFullSilenceAcrossPulls)
{
    // F=10, K=4：一个缺失槽（10 帧静音）会跨多个 pull，必须完整输出。
    auto jb = JitterBuffer::create(make_config(10, 10));
    ASSERT_TRUE(jb.has_value());

    // 推 0,1,3,4,5,6（缺 2），lead=7 ≥ 6 → 锚定在 0。
    for (std::uint64_t s : { 0ULL, 1ULL, 3ULL, 4ULL, 5ULL, 6ULL }) {
        ASSERT_TRUE(push_frame(**jb, s, 10));
    }

    // 排水到经过缺失槽即可（Fill 会在 lead 跌破 45% 后触发，故只断言 slot 0..3）。
    const auto bytes = drain(**jb, 4, 9);
    ASSERT_GE(bytes.size(), 32u * kFrameBytes);

    // 逐 slot 断言（每 slot 10 帧）。
    EXPECT_EQ(frame_fill(bytes, 0), 1u); // slot 0（fill=seq+1）
    EXPECT_EQ(frame_fill(bytes, 10), 2u); // slot 1
    EXPECT_EQ(frame_fill(bytes, 20), 0u); // slot 2 缺失 → 静音起点
    EXPECT_EQ(frame_fill(bytes, 29), 0u); // slot 2 缺失 → 静音终点（第 10 帧）
    EXPECT_EQ(frame_fill(bytes, 30), 4u); // slot 3 紧随其后 → 缺帧恰为 F=10 帧
}

TEST(JitterBufferTest, PushRejectsLateDuplicateAndAcceptsFarAhead)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s <= 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    drain(**jb, 4, 1); // 播放 seq 0 → play_seq=1

    EXPECT_FALSE(push_frame(**jb, 0, 4)); // 迟到（< play_seq）
    EXPECT_FALSE(push_frame(**jb, 1, 4)); // 重复（槽 1 仍 READY）
    EXPECT_TRUE(push_frame(**jb, 20, 4)); // 远超前（>= play_seq + N）→ 接受并请求 reanchor
    EXPECT_TRUE(push_frame(**jb, 6, 4)); // 正常新帧
}

TEST(JitterBufferTest, DeadlineHighSkipsToTarget)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());

    // 推满 10 帧 → lead=10 > 90% → deadline high。
    for (std::uint64_t s = 0; s < 10; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }

    JitterBufferPullResult r { };
    auto out = drain(**jb, 4, 1, &r);
    // skip = lead(10) - target_slots(6) = 4。
    EXPECT_EQ(r.skipped_slots, 4u);
    EXPECT_EQ(r.frames_filled, 4u);
    EXPECT_EQ(frame_fill(out, 0), 5u); // 跳过 0..3，从 seq 4（fill=5）开始
}

TEST(JitterBufferTest, WarningFillRepeatsReadySlotWithoutSilence)
{
    auto cfg = make_config(10, 4);
    cfg.normal_low = 0.55; // round(0.55*10)=6；target 仍为 6，lead=5 时进入 Fill。
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    // target=6，先用 6 个 slot 建 anchor，再消费 1 个 slot，
    // 使 lead=5 进入 warning-low..normal-low 的 Fill 区。
    for (std::uint64_t s = 0; s < 6; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }

    std::vector<std::byte> out(4 * kFrameBytes);
    auto anchored = (*jb)->pull(out);
    ASSERT_EQ(anchored.frames_filled, 4u);
    ASSERT_EQ(anchored.silence_frames, 0u);

    // 第二次 pull 请求 6 sample frames（跨越一个完整 slot + 当前 slot 的一部分）。
    // warning step=1 应让 seq=1 的 slot 额外播放一次，不能把整个 callback 填成静音。
    std::vector<std::byte> corrected(6 * kFrameBytes);
    const auto r = (*jb)->pull(corrected);
    EXPECT_EQ(r.frames_filled, 6u);
    EXPECT_EQ(r.silence_frames, 0u);
    for (std::uint32_t i = 0; i < 6; ++i) {
        EXPECT_EQ(frame_fill(corrected, i), 2u);
    }
    // 6 帧 = 1.5 slot；step=1 重播当前 slot：seq=1 正常播 4 帧 + 重播 2 帧，
    // play 仍停在 seq=1（replay 尚未完成）→ lead = 5-1+1 = 5 → 5/10。
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.5);
    EXPECT_EQ((*jb)->fill_corrected_slots(), 1u);

    // 下一次 2 帧 pull 正好补完 seq=1 剩余的 2 帧 replay，随后 advance 到 seq=2
    // （本次输出仍全部是 seq=1 数据）。
    std::vector<std::byte> resumed(2 * kFrameBytes);
    const auto r2 = (*jb)->pull(resumed);
    EXPECT_EQ(r2.frames_filled, 2u);
    EXPECT_EQ(r2.silence_frames, 0u);
    EXPECT_EQ(frame_fill(resumed, 0), 2u);
    EXPECT_EQ(frame_fill(resumed, 1), 2u);
}

TEST(JitterBufferTest, WarningFillUsesSlotQuantumAcrossPlaybackCallback)
{
    constexpr std::uint32_t slot_frames = 180;
    constexpr std::uint32_t callback_frames = 512;
    auto cfg = make_config(30, slot_frames);
    cfg.normal_low = 0.59; // round(0.59*30)=18；target=18，lead=17 时进入 Fill。
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s < 18; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, slot_frames));
    }

    // 建 anchor 并消费 seq=0，随后 lead=17 进入 warning Fill。
    std::vector<std::byte> warmup(static_cast<std::size_t>(slot_frames) * kFrameBytes);
    const auto warm = (*jb)->pull(warmup);
    ASSERT_EQ(warm.frames_filled, slot_frames);
    ASSERT_EQ(warm.silence_frames, 0u);

    // 512-frame playback callback 跨越 2.84 个 slot；step=1 只能产生 1 slot 的慢放校正，
    // 不能把整个 512 帧 callback 变成静音。
    std::vector<std::byte> corrected(static_cast<std::size_t>(callback_frames) * kFrameBytes);
    const auto r = (*jb)->pull(corrected);
    ASSERT_EQ(r.frames_filled, callback_frames);
    EXPECT_EQ(r.silence_frames, 0u);

    // seq=1 的 180 帧原音 + 180 帧重播，再继续消费 seq=2 的 152 帧。
    for (std::uint32_t i = 0; i < slot_frames * 2; ++i) {
        EXPECT_EQ(frame_fill(corrected, i), 2u);
    }
    for (std::uint32_t i = slot_frames * 2; i < callback_frames; ++i) {
        EXPECT_EQ(frame_fill(corrected, i), 3u);
    }
    EXPECT_EQ((*jb)->fill_corrected_slots(), 1u);
}

TEST(JitterBufferTest, WarningFillStepTwoRepeatsCurrentSlotTwice)
{
    auto cfg = make_config(10, 4);
    cfg.normal_low = 0.55; // lead=5 enters warning Fill after anchor.
    cfg.step_fn = [](const aqua::audio::WarningStepParams&, std::uint32_t) noexcept -> std::uint32_t {
        return 2;
    };
    auto jb = JitterBuffer::create(cfg);
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s < 6; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }

    std::vector<std::byte> anchored(4 * kFrameBytes);
    ASSERT_EQ((*jb)->pull(anchored).silence_frames, 0u);

    // 12 frames = 3 slots. step=2 means the current slot is played three times total:
    // one normal pass + two replay passes. No warning silence is generated.
    std::vector<std::byte> corrected(12 * kFrameBytes);
    const auto r = (*jb)->pull(corrected);
    EXPECT_EQ(r.frames_filled, 12u);
    EXPECT_EQ(r.silence_frames, 0u);
    for (std::uint32_t i = 0; i < 12; ++i) {
        EXPECT_EQ(frame_fill(corrected, i), 2u);
    }
    EXPECT_EQ((*jb)->fill_corrected_slots(), 2u);
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.4); // highest=5, play=2 => lead=4/10.
}

TEST(JitterBufferTest, WaterLevelReflectsLead)
{
    auto jb = JitterBuffer::create(make_config(10, 4));
    ASSERT_TRUE(jb.has_value());

    for (std::uint64_t s = 0; s < 5; ++s) {
        ASSERT_TRUE(push_frame(**jb, s, 4));
    }
    // 未启动：lead = 4-0+1 = 5 → 0.5。
    EXPECT_DOUBLE_EQ((*jb)->water_level(), 0.5);
}

} // namespace
