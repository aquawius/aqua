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

// ---- 尾部直方图（默认策略的抖动项输入）----
// 口径：只进走到 transit 计算的包（按序 + 时间轴有效），相对时延 = transit - base。

TEST(JitterEstimatorTest, TailP99NearZeroOnCleanLan)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    // 150 包（149 有效样本 ≥ 128 门限）：全落 0ms 桶。
    for (int i = 0; i < 150; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.tail_samples, 149u);
    EXPECT_DOUBLE_EQ(estimates.tail_p99_ms, 0.0);
}

TEST(JitterEstimatorTest, TailP99CapturesStallExcludedSpike)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    // 先过冷启动门（130 包 → 129 样本），再来一簇 spike。
    // 注意是"一簇"不是"一个"：n=130 时单个 spike 占 0.8% <1%，P99 看不见
    // 它——这正是噪声免疫的本意，不是 bug。25 个连续 +60ms 到达（每包都超
    // 50ms stall 阈值 → 进 stall 峰值、不进 J），每包 |到达-发送| = |70-10|
    // = 60ms（差分不累积），全落 60 桶，P99 = 60。
    for (int i = 0; i < 130; ++i) {
        feeder.packet();
    }
    for (int i = 0; i < 25; ++i) {
        feeder.packet(60.0);
    }
    const auto estimates = estimator.estimates();
    EXPECT_DOUBLE_EQ(estimates.tail_p99_ms, 60.0);
    EXPECT_EQ(estimates.tail_samples, 154u);
    // stall 样本确实没进 J（J 只吃了前面的干净包，均值 ≈0）。
    EXPECT_NEAR(estimates.jitter_ms, 0.0, 0.5);
    EXPECT_GT(estimates.stall_peak_ms, 0.0);
}

TEST(JitterEstimatorTest, TailP99ForgetsAsWindowSlides)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 130; ++i) {
        feeder.packet();
    }
    for (int i = 0; i < 25; ++i) {
        feeder.packet(60.0); // 一簇 spike 进窗（全落 60 桶）
    }
    ASSERT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, 60.0);
    // 窗 2048：再推 2048 个干净包，最老的 154 个样本（含整簇 spike）全部滑出。
    for (int i = 0; i < 2048; ++i) {
        feeder.packet();
    }
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.tail_samples, 2048u); // 满窗钳制
    EXPECT_DOUBLE_EQ(estimates.tail_p99_ms, 0.0); // 簇已遗忘
}

TEST(JitterEstimatorTest, TailIgnoresReorderedPackets)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 7; ++i) {
        feeder.packet(); // seq 0..6
    }
    feeder.skip(3); // seq 7,8,9 wire 缺失（游标走到 10）
    feeder.packet(); // seq 10：max 前进，7..9 成"窗内未见"
    // 迟到的 seq 8：behind=2 且位未置 → reordered，提前 return 不进 transit/尾部。
    estimator.observe(8, 8 * kFrames, kSsrcA, feeder.arrival_ns + kPacketNs);
    const auto estimates = estimator.estimates();
    EXPECT_EQ(estimates.reordered, 1u);
    // 7+1 包 - 首包基线 = 7 个有效样本（<128 门限，P99 发布 -1），未被乱序包污染。
    EXPECT_EQ(estimates.tail_samples, 7u);
    EXPECT_DOUBLE_EQ(estimates.tail_p99_ms, -1.0);
}

TEST(JitterEstimatorTest, TailIgnoresIsolatedSpikeByDesign)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 130; ++i) {
        feeder.packet();
    }
    // 单个 +60ms spike（占 1/130 <1%）：P99 看不见它是设计本意（噪声免疫），
    // 它由 stall_peak 项负责。不断言具体桶位，只断言不断言 60。
    feeder.packet(60.0);
    EXPECT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, 0.0);
    EXPECT_EQ(estimator.estimates().tail_samples, 130u);
}

TEST(JitterEstimatorTest, TailClearedOnReset)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 130; ++i) {
        feeder.packet();
    }
    for (int i = 0; i < 25; ++i) {
        feeder.packet(60.0);
    }
    ASSERT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, 60.0);
    estimator.reset();
    EXPECT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, -1.0);
    EXPECT_EQ(estimator.estimates().tail_samples, 0u);
}

TEST(JitterEstimatorTest, TransitLevelFollowsPathSteps)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 50; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(estimator.estimates().transit_step_events, 0u);
    EXPECT_LT(std::fabs(estimator.estimates().transit_level_ms), 1.0);
    // 单包迟到 30ms（< 50ms stall 门）：transit 跳 +30，记一次台阶。
    feeder.packet(30.0);
    EXPECT_EQ(estimator.estimates().transit_step_events, 1u);
    EXPECT_GT(estimator.estimates().last_transit_step_ms, 25.0);
    EXPECT_LT(estimator.estimates().last_transit_step_ms, 35.0);
    // 后续干净包：电平在 ~0.23s 时间常数内收敛到新平台（+30）。
    for (int i = 0; i < 200; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(estimator.estimates().transit_step_events, 1u);
    EXPECT_GT(estimator.estimates().transit_level_ms, 25.0);
    // reset 清电平与台阶计数（新流）。
    estimator.reset();
    EXPECT_EQ(estimator.estimates().transit_step_events, 0u);
    EXPECT_DOUBLE_EQ(estimator.estimates().transit_level_ms, 0.0);
}

TEST(JitterEstimatorTest, TransitStepIgnoresJitterNoise)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 200; ++i) {
        feeder.packet((i % 2 == 0) ? 0.5 : -0.5);
    }
    EXPECT_EQ(estimator.estimates().transit_step_events, 0u);
    EXPECT_LT(std::fabs(estimator.estimates().transit_level_ms), 1.0);
}

TEST(JitterEstimatorTest, TransitLevelResetsOnTimelineRebuild)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 50; ++i) {
        feeder.packet();
    }
    feeder.packet(30.0); // +30ms 台阶
    for (int i = 0; i < 200; ++i) {
        feeder.packet();
    }
    ASSERT_GT(estimator.estimates().transit_level_ms, 25.0);
    // 发送端时间轴重置（timestamp 回退）：锚点重建，电平同步归零，
    // 不能把"新锚点下的小 transit vs 旧电平"误判成反向台阶。
    feeder.timestamp -= 10000;
    feeder.packet();
    for (int i = 0; i < 100; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(estimator.estimates().transit_step_events, 1u);
    EXPECT_LT(std::fabs(estimator.estimates().transit_level_ms), 2.0);
}

TEST(JitterEstimatorTest, TransitStepThresholdScalesWithPacketSize)
{
    // F=175@48k → 3.646ms/包 → 阈值 ≈ 9.1ms。feeder 的到达按 3.646ms 步进
    // （arrival_jitter 抵消 feeder 自带的 10ms 步长），时间轴与包周期对齐。
    JitterEstimator estimator(kRate, 175);
    Feeder feeder { estimator };
    for (int i = 0; i < 50; ++i) {
        feeder.packet(-6.354, 175.0);
    }
    EXPECT_EQ(estimator.estimates().transit_step_events, 0u);
    // +8ms（≈2.2 包）不记；+11ms（≈3.0 包）记一次。
    feeder.packet(1.646, 175.0);
    EXPECT_EQ(estimator.estimates().transit_step_events, 0u);
    feeder.packet(4.646, 175.0);
    EXPECT_EQ(estimator.estimates().transit_step_events, 1u);
    EXPECT_GT(estimator.estimates().last_transit_step_ms, 10.0);
    EXPECT_LT(estimator.estimates().last_transit_step_ms, 12.0);
}

TEST(JitterEstimatorTest, TransitStepCountsStallDrivenJumps)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    for (int i = 0; i < 50; ++i) {
        feeder.packet();
    }
    // 60ms 间隙：既是 stall（>50ms 门），transit 也跳 +50 → 两行各记一次，
    // 对照即区分"到达断了"与"路径跳了"（同一事件的两面）。
    feeder.packet(60.0);
    EXPECT_EQ(estimator.estimates().stall_events, 1u);
    EXPECT_EQ(estimator.estimates().transit_step_events, 1u);
    EXPECT_GT(estimator.estimates().last_transit_step_ms, 45.0);
}

TEST(JitterEstimatorTest, TailGatedUntilMinSamples)
{
    JitterEstimator estimator(kRate, kFrames);
    Feeder feeder { estimator };
    // 50 包（<128 门限）：P99 发布 -1（无尾部数据），controller 回退 k×J。
    // 冷启动单个 spike 就是 P99，直接驱动会把启动期一次断流顶到顶。
    for (int i = 0; i < 50; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(estimator.estimates().tail_samples, 49u);
    EXPECT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, -1.0);
    // 过门限后恢复正常发布（干净网 → 0）。
    for (int i = 0; i < 100; ++i) {
        feeder.packet();
    }
    EXPECT_EQ(estimator.estimates().tail_samples, 149u);
    EXPECT_DOUBLE_EQ(estimator.estimates().tail_p99_ms, 0.0);
}

} // namespace
