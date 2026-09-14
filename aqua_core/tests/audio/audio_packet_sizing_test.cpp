// 包尺寸（auto-F）策略单测：锁定 MTU-1400 / 5ms 包时长上限 / 0.5ms 包时长下限
// 这一套口径。这是 pacing 改造与 MTU 1440→1400 收敛后的核心推导，此前只有
// 间接覆盖（server_runtime / cli 各自内联了一份同口径逻辑），缺独立的单元测试。
//
// 测试只依赖 audio/audio_format.h 里的两个纯函数：
//   - frame_count_for_budget(fmt, budget)  ：F 的 MTU 预算侧（floor(budget / frame_bytes)）
//   - frame_count_for_duration(fmt, ms)     ：F 的包时长上限侧（floor(sr × ms / 1000)）
// auto-F = min(预算侧, 时长侧)；再按 packet_ms = F×1000/sr 落到 0.5ms 下限之上，
// 否则（极端多声道/高位深/高采样率）直接拒绝——这类格式在 1400B 预算下包率过高、
// pacing 不可实现、交接队列必然溢出，启动期拒掉比静默跑起来疯狂丢帧好。

#include "aqua/audio/audio_format.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/runtime/runtime_config.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::frame_count_for_budget;
using aqua::audio::frame_count_for_duration;
using aqua::config::UDP_AUDIO_MAX_PACKET_MS;
using aqua::config::UDP_AUDIO_MIN_PACKET_MS;
using aqua::config::UDP_AUDIO_PAYLOAD_BYTES;

AudioFormat fmt(AudioEncoding enc, std::uint32_t ch, std::uint32_t sr)
{
    return AudioFormat { enc, ch, sr };
}

// 复刻 ServerRuntime / cli 的 auto-F 决策（两份实现同口径），用于在单测里
// 锁定"最终给出的 F"与"是否应被下限拒绝"。返回 0 表示应拒绝。
std::uint32_t resolve_auto_f(const AudioFormat& f)
{
    const auto budget = frame_count_for_budget(f, UDP_AUDIO_PAYLOAD_BYTES);
    const auto duration = frame_count_for_duration(f, UDP_AUDIO_MAX_PACKET_MS);
    if (budget == 0 || duration == 0) {
        return 0;
    }
    const auto frames = std::min(budget, duration);
    const double packet_ms = static_cast<double>(frames) * 1000.0
        / static_cast<double>(f.sample_rate);
    return packet_ms >= UDP_AUDIO_MIN_PACKET_MS ? frames : 0;
}

double packet_ms_of(std::uint32_t frames, std::uint32_t sr)
{
    return static_cast<double>(frames) * 1000.0 / static_cast<double>(sr);
}

// ---- MTU 预算侧：F = floor(1400 / frame_bytes) ----

TEST(AudioPacketSizingTest, BudgetFramesMatchFloorOfPayloadOverFrameBytes)
{
    // stereo F32 @48k：frame_bytes = 8 → floor(1400/8) = 175（默认档 F）
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_F32LE, 2, 48000),
                  UDP_AUDIO_PAYLOAD_BYTES),
        175u);
    // mono F32 @44.1k：frame_bytes = 4 → 350
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_F32LE, 1, 44100),
                  UDP_AUDIO_PAYLOAD_BYTES),
        350u);
    // mono S16 @48k：frame_bytes = 2 → 700
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_S16LE, 1, 48000),
                  UDP_AUDIO_PAYLOAD_BYTES),
        700u);
    // stereo S16 @48k：frame_bytes = 4 → 350
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_S16LE, 2, 48000),
                  UDP_AUDIO_PAYLOAD_BYTES),
        350u);
    // 7.1ch F32 @48k：frame_bytes = 32 → floor(1400/32) = 43
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_F32LE, 8, 48000),
                  UDP_AUDIO_PAYLOAD_BYTES),
        43u);
    // 7.1ch F32 @192k：frame_bytes = 32 → 43（同帧字节，与采样率无关）
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_F32LE, 8, 192000),
                  UDP_AUDIO_PAYLOAD_BYTES),
        43u);
}

TEST(AudioPacketSizingTest, BudgetBelowOneFrameReturnsZero)
{
    // 单帧字节数超过 1400 预算（极端高码率）：放不下一帧 → 0。
    // PCM_S24LE 7.1ch @48k：frame_bytes = 3×8 = 24 → 能放（1400/24=58），
    // 用更小预算触发"放不下一帧"。
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_S24LE, 8, 48000), 20), 0u);
    // 预算恰好等于 frame_bytes → 1 帧。
    EXPECT_EQ(frame_count_for_budget(fmt(AudioEncoding::PCM_F32LE, 2, 48000), 8), 1u);
}

TEST(AudioPacketSizingTest, BudgetRejectsInvalidFormat)
{
    EXPECT_EQ(frame_count_for_budget(AudioFormat { }, UDP_AUDIO_PAYLOAD_BYTES), 0u);
    auto bad = fmt(AudioEncoding::PCM_F32LE, 65, 48000); // 声道超上限 → 非法
    EXPECT_EQ(frame_count_for_budget(bad, UDP_AUDIO_PAYLOAD_BYTES), 0u);
}

// ---- 包时长上限侧：F = floor(sr × max_packet_ms / 1000) ----

TEST(AudioPacketSizingTest, DurationFramesMatchFloorOfRateTimesMaxMs)
{
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_F32LE, 2, 48000),
                  UDP_AUDIO_MAX_PACKET_MS),
        240u); // 48000 × 5 / 1000 = 240
    // 44.1k：44100 × 5 / 1000 = 220.5 → floor 220（验证向下取整，不是四舍五入）
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_F32LE, 1, 44100),
                  UDP_AUDIO_MAX_PACKET_MS),
        220u);
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_S16LE, 1, 16000),
                  UDP_AUDIO_MAX_PACKET_MS),
        80u);
    // 下限边界 0.5ms @48k：48000 × 0.5 / 1000 = 24
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_F32LE, 2, 48000),
                  UDP_AUDIO_MIN_PACKET_MS),
        24u);
}

TEST(AudioPacketSizingTest, DurationRejectsNonPositiveOrInvalid)
{
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_F32LE, 2, 48000), 0.0), 0u);
    EXPECT_EQ(frame_count_for_duration(fmt(AudioEncoding::PCM_F32LE, 2, 48000), -1.0), 0u);
    EXPECT_EQ(frame_count_for_duration(AudioFormat { }, UDP_AUDIO_MAX_PACKET_MS), 0u);
}

// ---- 组合 auto-F = min(预算侧, 时长侧)，并验证包时长落在下限之上 ----

TEST(AudioPacketSizingTest, CanonicalStereoF32_48kResolvesTo175)
{
    // 默认档：预算 175、时长 240 → min = 175；packet_ms = 175×1000/48000 ≈ 3.646ms。
    const auto f = fmt(AudioEncoding::PCM_F32LE, 2, 48000);
    ASSERT_EQ(resolve_auto_f(f), 175u);
    EXPECT_NEAR(packet_ms_of(175, 48000), 3.646, 1e-3);
    // 同时 >= 0.5ms 下限（否则会被拒绝）。
    EXPECT_GE(packet_ms_of(175, 48000), UDP_AUDIO_MIN_PACKET_MS);
}

TEST(AudioPacketSizingTest, DurationCapDominantForLowFrameRateFormats)
{
    // mono S16 @48k：预算 700、时长 240 → 时长侧封顶到 240，packet_ms = 5.0ms。
    const auto f = fmt(AudioEncoding::PCM_S16LE, 1, 48000);
    ASSERT_EQ(resolve_auto_f(f), 240u);
    EXPECT_NEAR(packet_ms_of(240, 48000), 5.0, 1e-6);
    // 44.1k stereo S16：预算 350、时长 220 → 220，packet_ms ≈ 4.9886ms。
    const auto f2 = fmt(AudioEncoding::PCM_S16LE, 2, 44100);
    ASSERT_EQ(resolve_auto_f(f2), 220u);
    EXPECT_NEAR(packet_ms_of(220, 44100), 4.9886, 1e-3);
}

TEST(AudioPacketSizingTest, ExtremeFormatRejectedByMinDurationFloor)
{
    // 7.1ch F32 @192k：预算 43、时长 960 → min = 43，但 packet_ms = 43×1000/192000
    // = 0.224ms < 0.5ms 下限 → 必须被拒绝（返回 0）。这正是"1400B 预算下无法
    // 可靠传输"的格式：包率 4464/s，pacing 间隔低于可实现精度、交接队列持续溢出。
    const auto f = fmt(AudioEncoding::PCM_F32LE, 8, 192000);
    ASSERT_EQ(frame_count_for_budget(f, UDP_AUDIO_PAYLOAD_BYTES), 43u);
    ASSERT_EQ(resolve_auto_f(f), 0u)
        << "极端格式应被 0.5ms 包时长下限拒绝，而非给出一个不可实现的 F";
}

TEST(AudioPacketSizingTest, PacketDurationMonotoneInFrames)
{
    // packet_ms = F×1000/sr，对同一格式随 F 单调递增——包率（1000/packet_ms）
    // 与 JB 每槽粒度都单调，便于推理。
    const auto f = fmt(AudioEncoding::PCM_F32LE, 2, 48000);
    EXPECT_LT(packet_ms_of(175, f.sample_rate), packet_ms_of(240, f.sample_rate));
    EXPECT_GT(1000.0 / packet_ms_of(175, f.sample_rate), 1000.0 / packet_ms_of(240, f.sample_rate));
}

TEST(AudioPacketSizingTest, PayloadBudgetIs1400Not1440)
{
    // 回归锁定：MTU payload 是 1400（IPv6 1440 再让 40B 给隧道/封装余量），
    // 不是最早的 1440。F=176 @ stereo F32 会算出 1408 字节 > 1400，必须被预算侧
    // 截断在 175，否则单包越界分片。
    EXPECT_EQ(UDP_AUDIO_PAYLOAD_BYTES, 1400u);
    const auto f = fmt(AudioEncoding::PCM_F32LE, 2, 48000);
    // floor(1440/8) = 180，但若预算是 1440 会得到 180；实测应为 175（预算 1400）。
    EXPECT_EQ(frame_count_for_budget(f, UDP_AUDIO_PAYLOAD_BYTES), 175u);
}

} // namespace
