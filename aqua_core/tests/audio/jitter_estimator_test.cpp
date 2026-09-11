// Phase 0 合成 trace 测试：JitterEstimator 纯观察语义。
// 确定性 (seq16, timestamp, arrival_ns) 序列，无 socket、无 JB、无 playback。
// sample_rate=48000, F=480 → 包时长 10ms。

#include "aqua/audio/buffer/jitter_estimator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace {

using aqua::audio::JitterEstimator;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kFrames = 480;
constexpr std::uint32_t kSsrcA = 0x11111111u;
constexpr std::uint32_t kSsrcB = 0x22222222u;
constexpr std::int64_t kPacketNs = 10'000'000; // 10ms
constexpr std::int64_t kT0 = 1'000'000'000;

// 确定性 feeder：维护 seq/ts/arrival 三游标。
struct Feeder {
    JitterEstimator& estimator;
    std::uint16_t seq = 0;
    std::uint32_t timestamp = 0;
    std::int64_t arrival_ns = kT0;
    std::uint32_t ssrc = kSsrcA;
    double ts_fraction = 0.0; // timestamp 小数累积（漂移场景）

    void packet(double arrival_jitter_ms = 0.0, double ts_advance_frames = kFrames)
    {
        ts_fraction += ts_advance_frames;
        const auto whole = static_cast<std::uint32_t>(ts_fraction);
        ts_fraction -= whole;
        timestamp += whole;
        arrival_ns += kPacketNs + static_cast<std::int64_t>(arrival_jitter_ms * 1'000'000.0);
        estimator.observe(seq, timestamp, ssrc, arrival_ns);
        ++seq; // uint16_t 自然回绕
    }

    void skip(std::uint16_t count)
    {
        // 丢包：游标前进但不观测（wire 上缺失）。
        for (std::uint16_t i = 0; i < count; ++i) {
            timestamp += kFrames;
            arrival_ns += kPacketNs;
            ++seq;
        }
    }
};

TEST(JitterEstimatorTest, CleanLanHasZeroJitterAndStableTransit)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 50; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 50u);
    EXPECT_EQ(estimates.gap_events, 0u);
    EXPECT_EQ(estimates.duplicates, 0u);
    EXPECT_EQ(estimates.reordered, 0u);
    EXPECT_EQ(estimates.late, 0u);
    EXPECT_NEAR(estimates.jitter_ms, 0.0, 1e-6);
    EXPECT_NEAR(estimates.transit_ms, 0.0, 1e-6);
    EXPECT_NEAR(estimates.base_delay_ms, 0.0, 1e-6);
    EXPECT_NEAR(estimates.arrival_interval_ms, 10.0, 1e-6);
}

TEST(JitterEstimatorTest, AlternatingOneMillisecondJitterConvergesToOne)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 200; ++i) {
        feeder.packet(i % 2 == 0 ? 1.0 : -1.0);
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 200u);
    // RFC J 对 ±1ms 交替收敛到 ~1ms（1/16 增益，200 包充分收敛）。
    EXPECT_GT(estimates.jitter_ms, 0.5);
    EXPECT_LT(estimates.jitter_ms, 1.5);
}

TEST(JitterEstimatorTest, IsolatedSpikeBumpsAndDecaysJitter)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 20; ++i) {
        feeder.packet();
    }
    feeder.packet(30.0); // +30ms 孤立尖峰
    EXPECT_GT(estimator.estimates().jitter_ms, 1.0);
    for (int i = 0; i < 100; ++i) {
        feeder.packet();
    }
    // 尖峰被 1/16 增益洗掉。
    EXPECT_LT(estimator.estimates().jitter_ms, 0.5);
    EXPECT_EQ(estimator.estimates().packets, 121u);
}

TEST(JitterEstimatorTest, BurstLossOfFiveCountsOneGapEvent)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 30; ++i) {
        feeder.packet();
    }
    feeder.skip(5);
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 40u);
    EXPECT_EQ(estimates.gap_events, 1u);
    EXPECT_EQ(estimates.missing_packets, 5u);
}

TEST(JitterEstimatorTest, TwoPacketReorderCountsReorderedNotGapRepair)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    // 11 先到（跳跃记 gap，缺 10），10 后到（窗内落后记 reordered）。
    const auto seq_save = feeder.seq;
    feeder.seq = static_cast<std::uint16_t>(seq_save + 1);
    feeder.packet();
    feeder.seq = seq_save;
    feeder.packet();
    // 游标续到 12（11 已见过，避开 accidental duplicate）。
    feeder.seq = static_cast<std::uint16_t>(seq_save + 2);
    for (int i = 0; i < 5; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.gap_events, 1u);
    EXPECT_EQ(estimates.missing_packets, 1u);
    EXPECT_EQ(estimates.reordered, 1u);
    EXPECT_EQ(estimates.duplicates, 0u);
}

TEST(JitterEstimatorTest, ImmediateDuplicateCountsDuplicate)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 5; ++i) {
        feeder.packet();
    }
    // seq 5 重发一次（游标回退一格重放）。
    feeder.seq = static_cast<std::uint16_t>(feeder.seq - 1);
    feeder.timestamp -= kFrames;
    feeder.packet();
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 6u);
    EXPECT_EQ(estimates.duplicates, 1u);
    EXPECT_EQ(estimates.gap_events, 0u);
}

TEST(JitterEstimatorTest, SteadySenderClockDriftKeepsJitterSmall)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    // +50ppm：每包 ts 多走 480*50e-6 = 0.024 sample。
    for (int i = 0; i < 500; ++i) {
        feeder.packet(0.0, kFrames * 1.00005);
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 500u);
    EXPECT_EQ(estimates.gap_events, 0u);
    EXPECT_LT(estimates.jitter_ms, 0.5);
    // transit 缓慢漂移（500 包 × 10ms × 50ppm = 0.25ms；发送时钟偏快，
    // dts > darr，故为负），base 钉在起点。
    EXPECT_LT(estimates.transit_ms, -0.1);
    EXPECT_GT(estimates.transit_ms, -0.5);
}

TEST(JitterEstimatorTest, TimestampWrapKeepsTransitContinuous)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    feeder.timestamp = 0xFFFFFFFFu - 5 * kFrames;
    for (int i = 0; i < 20; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 20u);
    EXPECT_EQ(estimates.gap_events, 0u);
    EXPECT_LT(std::fabs(estimates.transit_ms), 1.0);
    EXPECT_LT(estimates.jitter_ms, 1e-6);
}

TEST(JitterEstimatorTest, SequenceWrapExtendsContinuously)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    feeder.seq = 65530;
    for (int i = 0; i < 40; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.packets, 40u);
    EXPECT_EQ(estimates.gap_events, 0u);
    EXPECT_EQ(estimates.missing_packets, 0u);
}

TEST(JitterEstimatorTest, SsrcChangeResetsStreamState)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    feeder.ssrc = kSsrcB;
    for (int i = 0; i < 5; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    // 新流：计数从零起（旧流 10 包已丢弃），无虚假 gap。
    EXPECT_EQ(estimates.packets, 5u);
    EXPECT_EQ(estimates.gap_events, 0u);
}

// stall 与抖动分离（kFrames=480 → 包周期 10ms，默认阈值 5 包周期 = 50ms）。
TEST(JitterEstimatorTest, StallIsExcludedFromJitter)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 40; ++i) {
        feeder.packet(); // 干净流：J→0
    }
    ASSERT_LT(estimator.estimates().jitter_ms, 0.5);

    // 一次 100ms 断流：interval = 10 + 100 = 110ms > 50ms 阈值 → stall，
    // 不进 J。若进了 J，单样本会把它从 ~0 顶到 (100-0)/16 ≈ 6.25ms。
    feeder.packet(100.0);
    auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.stall_events, 1u);
    EXPECT_DOUBLE_EQ(estimates.last_stall_gap_ms, 110.0);
    EXPECT_LT(estimates.jitter_ms, 1.0) << "stall 样本不得进入 RFC 3550 J";

    // stall 后干净流继续：J 保持低位（不被那次断流拖高）。
    for (int i = 0; i < 20; ++i) {
        feeder.packet();
    }
    EXPECT_LT(estimator.estimates().jitter_ms, 1.0);
}

TEST(JitterEstimatorTest, SubThresholdGapStillCountsAsJitter)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 40; ++i) {
        feeder.packet();
    }
    ASSERT_LT(estimator.estimates().jitter_ms, 0.5);

    // 30ms 抖动间隙（interval = 10 + 20 = 30ms < 50ms 阈值）→ 不是 stall，
    // 正常进 J。
    feeder.packet(20.0);
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.stall_events, 0u);
    EXPECT_GT(estimates.jitter_ms, 0.5) << "阈值以下的间隙仍属抖动，必须进 J";
}

// 关闭 stall 剔除（阈值 0）：一切间隙都进 J（退回 RFC 3550 原始口径）。
TEST(JitterEstimatorTest, StallDetectionCanBeDisabled)
{
    JitterEstimator estimator(kRate, kFrames, 0.0);
    Feeder feeder { estimator };
    for (int i = 0; i < 40; ++i) {
        feeder.packet();
    }
    feeder.packet(100.0);
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.stall_events, 0u);
    EXPECT_GT(estimates.jitter_ms, 1.0);
}

// stall 峰值跟踪（NetEq 式 peak detection）：stall 刷新为 max，无 stall 时
// 按 10ms/s 线性衰减。target 的 margin 峰值项靠它覆盖被 stall 门剔除的尾部。
TEST(JitterEstimatorTest, StallRefreshesPeakAndDecayForgetsIt)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    // 100ms 断流 → interval 110ms > 50ms 阈值 → 峰值 = 110。
    feeder.packet(100.0);
    EXPECT_DOUBLE_EQ(estimator.estimates().stall_peak_ms, 110.0);

    // 10 个干净包（100ms）衰减 1.0ms；再来一个较小的 stall（interval 60ms）
    // 不拉低峰值（decayed max，不是最近值）：60ms 间隔先衰减 0.6ms。
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    feeder.packet(50.0);
    EXPECT_EQ(estimator.estimates().stall_events, 2u);
    EXPECT_NEAR(estimator.estimates().stall_peak_ms, 108.4, 0.01);

    // 无 stall 继续 10s（1000 包 @10ms）：衰减 10ms/s × 10s = 100ms。
    for (int i = 0; i < 1000; ++i) {
        feeder.packet();
    }
    EXPECT_NEAR(estimator.estimates().stall_peak_ms, 8.4, 0.01);
}

// 亚阈值间隙（进 J 不进 stall）不刷峰值；峰值只靠衰减缓慢回落。
TEST(JitterEstimatorTest, SubThresholdGapDoesNotRefreshPeak)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    feeder.packet(20.0); // interval 30ms < 50ms 阈值 → 非 stall
    EXPECT_DOUBLE_EQ(estimator.estimates().stall_peak_ms, 0.0);
}

TEST(JitterEstimatorTest, StallPeakClearedOnReset)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 10; ++i) {
        feeder.packet();
    }
    feeder.packet(100.0);
    ASSERT_DOUBLE_EQ(estimator.estimates().stall_peak_ms, 110.0);
    estimator.reset();
    EXPECT_DOUBLE_EQ(estimator.estimates().stall_peak_ms, 0.0);
}

} // namespace
