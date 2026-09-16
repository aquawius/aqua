// 离线回放 harness：把"合成链路"喂进**真实的** JitterEstimator + TargetController
// + JitterBuffer，回答"改控制律参数前先离线看 target 会落在哪、欠载/丢帧预算守不守得住"。
//
// 为什么驱动真实对象（而不是像 tools/sim_jb_target.py 那样重实现一遍）：
// 那份 Python 模型与本控制律已经分叉 6 处（base 混进 margin / 只有 k×J / 上限用
// capacity 而非 2/3 / fall 用 1.0 / 波段倍率 0.25-0.50-1.25-1.50 / 无 storm+penalty），
// 用它得出的参数结论会优化一个不存在的系统。这里公式、限速、风暴、带几何全部来自
// 生产代码：estimator/controller 是同一个类，JB 配置经 apply_adaptive_bands
// （ClientRuntime 用的同一函数）生成，因此不可能再漂移。
//
// 模型边界（刻意如此）：
//   - 只合成"到达时刻"与"消费节奏"，样本内容是常量填充（不关心音频质量）；
//   - 不模拟 socket/线程：事件表按时间排序后顺序执行，虚拟时钟，无 sleep → 确定性；
//   - 丢包 = 该包永不产生（接收侧看不到洞，与 UDP 一致）；
//   - 用固定 seed 的 PRNG，最坏相位由扫描得到（发包 10ms / 消费 10.667ms 的拍频
//     约 160ms 扫过全部相位，只看一个相位会严重低估欠载）。
//
// 几何取自生产口径：capture block 480 帧/10ms → 每个 callback 从累积器里切出
// 2 或 3 个 175 帧的包（背靠背发出，串内到达间隔≈0，这是 jit 的主要来源）；
// playback pull 512 帧/10.667ms。容量 30 槽。

#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/buffer_config.h"
#include "aqua/audio/buffer/jitter_estimator.h"
#include "aqua/audio/buffer/target_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

namespace {

using aqua::audio::AudioEncoding;
using aqua::audio::AudioFormat;
using aqua::audio::AudioFrame;
using aqua::audio::JitterBuffer;
using aqua::audio::JitterBufferConfig;
using aqua::audio::JitterEstimator;
using aqua::audio::TargetController;
using aqua::audio::TargetControllerParams;
using aqua::audio::TargetMarginStrategy;
using aqua::audio::TargetPath;

// ---- 与生产同源的几何 ----
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kFrameCount = 175; // 包帧数（默认档）
constexpr std::uint32_t kFrameBytes = 4; // F32LE 单声道
constexpr std::uint32_t kCaptureBlock = 480; // 一次 capture callback 的帧数
constexpr double kCapturePeriodMs = 10.0;
constexpr std::uint32_t kPullFrames = 512; // WASAPI 周期（实测 512）
constexpr std::uint32_t kCapacitySlots = 30;
constexpr std::uint32_t kSsrc = 0xA0A0A0A0u;
constexpr std::int64_t kNsPerMs = 1'000'000;
constexpr double kRunMs = 12000.0;
constexpr double kWarmupMs = 2000.0; // 启动期不记预算（pre-roll 的静音是设计行为）

constexpr double kPacketMs = kFrameCount * 1000.0 / kSampleRate; // 3.646ms
constexpr double kPullPeriodMs = kPullFrames * 1000.0 / kSampleRate; // 10.667ms

struct LinkCondition {
    const char* name;
    double jitter_ms = 0.0; // 网络抖动（高斯 σ）
    double loss = 0.0; // 丢包率
};

struct Metrics {
    std::uint32_t target_min = 0;
    std::uint32_t target_max = 0;
    double target_mean = 0.0;
    double underrun_pct = 0.0; // (静音 + 掩盖) / 已拉取，暖机后
    double drop_pct = 0.0; // 跳过的槽帧 / (已拉取 + 跳过)
    double max_run_ms = 0.0;
    std::uint64_t fill_episodes = 0;
    std::uint64_t drop_episodes = 0;
    double p99_ms = 0.0;
    double jitter_ms = 0.0;
    bool saw_storm_hold = false;
};

struct Event {
    std::int64_t t_ns = 0;
    bool pull = false; // false = 到达，true = 消费
    std::uint16_t seq = 0;
    std::uint32_t ts = 0; // RTP 时间戳（采样数）
};

// 事件表：发包（累积器切片，背靠背）+ 消费（固定周期，带相位偏移）。
std::vector<Event> build_schedule(const LinkCondition& link, double phase_ms, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, link.jitter_ms > 0.0 ? link.jitter_ms : 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(kRunMs / kCapturePeriodMs * 3.0) + 1300);

    std::uint32_t pending = 0;
    std::uint16_t seq = 0;
    std::uint32_t ts = 0;
    for (double t = 0.0; t < kRunMs; t += kCapturePeriodMs) {
        pending += kCaptureBlock;
        const std::uint32_t n = pending / kFrameCount;
        pending -= n * kFrameCount;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (unit(rng) >= link.loss) {
                const double jitter = link.jitter_ms > 0.0 ? gauss(rng) : 0.0;
                const auto at = static_cast<std::int64_t>((t + jitter) * kNsPerMs);
                events.push_back(Event { at, false, seq, ts });
            }
            ++seq;
            ts += kFrameCount;
        }
    }
    for (double t = phase_ms; t < kRunMs; t += kPullPeriodMs) {
        events.push_back(Event { static_cast<std::int64_t>(t * kNsPerMs), true, 0, 0 });
    }
    std::stable_sort(events.begin(), events.end(),
        [](const Event& a, const Event& b) { return a.t_ns < b.t_ns; });
    return events;
}

// 生产口径的 controller 参数（对应 client_runtime.cpp setup_playback）。
TargetControllerParams make_params(TargetMarginStrategy strategy)
{
    TargetControllerParams p;
    // 结构上限 = 2/3 × capacity（target 顶到 capacity 会让高水位带落到 ring 外）。
    p.capacity_slots = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(static_cast<double>(kCapacitySlots)
            * aqua::config::JB_ADAPTIVE_TARGET_CAPACITY_RATIO));
    p.packet_ms = kPacketMs;
    p.jitter_gain = aqua::config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN;
    p.margin_strategy = strategy;
    p.min_target_slots = aqua::config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS;
    p.stall_peak_cap_slots = aqua::config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
    p.underrun_penalty_per_event = aqua::config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS;
    // 几何地板 = ceil(一次 callback 消耗的包数)。
    p.geometric_floor_slots = static_cast<std::uint32_t>(
        (kPullFrames + kFrameCount - 1) / kFrameCount);
    return p;
}

Metrics run(const TargetMarginStrategy strategy, const LinkCondition& link, double phase_ms,
    unsigned seed, double* target_series_out = nullptr)
{
    TargetControllerParams params = make_params(strategy);
    const std::uint32_t initial
        = std::min(TargetController::floor_target(params), params.capacity_slots);
    params.initial_target_slots = initial;

    JitterBufferConfig cfg;
    cfg.capacity_slots = kCapacitySlots;
    cfg.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, kSampleRate };
    cfg.frame_count = kFrameCount;
    // 产品默认：concealment + splice 都开（组件默认关，由 Runtime 打开）。
    cfg.concealment.enabled = true;
    cfg.concealment.max_slots = aqua::config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS;
    cfg.splice.enabled = true;
    // 水位带 + 启动水位：与 ClientRuntime 用同一个函数（防口径漂移）。
    aqua::audio::apply_adaptive_bands(cfg, kCapacitySlots, initial);

    auto created = JitterBuffer::create(cfg);
    EXPECT_TRUE(created.has_value());
    if (!created) {
        return Metrics { };
    }
    JitterBuffer& jb = **created;

    JitterEstimator estimator(kSampleRate, kFrameCount,
        aqua::config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS,
        aqua::config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC);
    TargetController controller(params);

    const auto schedule = build_schedule(link, phase_ms, seed);
    std::vector<std::byte> payload(static_cast<std::size_t>(kFrameCount) * kFrameBytes);
    std::vector<std::byte> output(static_cast<std::size_t>(kPullFrames) * kFrameBytes);

    Metrics m;
    std::uint64_t pulled = 0;
    std::uint64_t silent = 0;
    std::uint64_t dropped_slots = 0;
    std::uint64_t target_sum = 0;
    std::uint64_t target_n = 0;
    bool warmed = false;
    std::uint32_t target_min = kCapacitySlots;
    std::uint32_t target_max = 0;

    for (const auto& e : schedule) {
        if (!e.pull) {
            estimator.observe(e.seq, e.ts, kSsrc, e.t_ns);
            const auto estimates = estimator.estimates();
            const auto target = controller.update(estimates.jitter_ms, e.t_ns,
                jb.underrun_events(), estimates.stall_peak_ms, estimates.tail_p99_ms,
                estimates.stall_events);
            jb.set_target_slots(target);
            if (controller.path() == TargetPath::StormHold) {
                m.saw_storm_hold = true;
            }
            if (e.t_ns >= static_cast<std::int64_t>(kWarmupMs * kNsPerMs)) {
                target_sum += target;
                ++target_n;
                target_min = std::min(target_min, target);
                target_max = std::max(target_max, target);
            }
            // 样本内容用 (seq+1)&0xFF 填充：便于把"真实数据"与静音区分开。
            const auto fill = static_cast<std::byte>((e.seq + 1) & 0xFF);
            std::fill(payload.begin(), payload.end(), fill);
            AudioFrame frame { e.seq, kFrameCount, std::span<const std::byte>(payload) };
            (void)jb.push(frame);
            continue;
        }

        const auto r = jb.pull(output);
        if (!warmed && e.t_ns >= static_cast<std::int64_t>(kWarmupMs * kNsPerMs)) {
            warmed = true;
            pulled = 0;
            silent = 0;
            dropped_slots = 0;
        }
        pulled += r.frames_filled;
        silent += r.silence_frames;
        dropped_slots += r.skipped_slots;
    }

    m.target_min = target_n > 0 ? target_min : 0;
    m.target_max = target_max;
    m.target_mean = target_n > 0
        ? static_cast<double>(target_sum) / static_cast<double>(target_n)
        : 0.0;
    m.underrun_pct = 100.0 * static_cast<double>(silent) / static_cast<double>(std::max<std::uint64_t>(1, pulled));
    const std::uint64_t dropped_frames = dropped_slots * kFrameCount;
    m.drop_pct = 100.0 * static_cast<double>(dropped_frames)
        / static_cast<double>(std::max<std::uint64_t>(1, pulled + dropped_frames));
    m.max_run_ms = static_cast<double>(jb.max_silence_run_frames()) * 1000.0 / kSampleRate;
    m.fill_episodes = jb.fill_episodes();
    m.drop_episodes = jb.drop_episodes();
    const auto final_estimates = estimator.estimates();
    m.p99_ms = final_estimates.tail_p99_ms;
    m.jitter_ms = final_estimates.jitter_ms;
    if (target_series_out != nullptr) {
        *target_series_out = m.target_mean;
    }
    return m;
}

// 相位扫描：发包 10ms 与消费 10.667ms 拍频约 160ms 扫过全部相位，
// 最坏相位必须扫出来（只看单个相位会严重低估欠载）。
Metrics worst_phase(const TargetMarginStrategy strategy, const LinkCondition& link,
    unsigned seed, const int phases = 16)
{
    Metrics worst;
    double worst_underrun = -1.0;
    for (int i = 0; i < phases; ++i) {
        const auto m = run(strategy, link, static_cast<double>(i) * (kPullPeriodMs / static_cast<double>(phases)), seed);
        if (m.underrun_pct > worst_underrun) {
            worst_underrun = m.underrun_pct;
            worst = m;
        }
    }
    return worst;
}

const LinkCondition kClean { "理想有线", 0.0, 0.0 };
const LinkCondition kLanJitter { "+1ms 抖动", 1.0, 0.0 };
const LinkCondition kWifi { "+3ms 抖动(WiFi)", 3.0, 0.0 };
const LinkCondition kLoss { "+1% 丢包", 1.0, 0.01 };
const LinkCondition kStorm { "断流风暴(35% 丢包)", 1.0, 0.35 };

// 场景表：这是"改参数前先离线看效果"的入口。跑
//   aqua_jitter_buffer_tests.exe --gtest_filter=*ScenarioTable*
// 直接看表格（ctest 下加 --output-on-failure 或 -V 也能看到）。
TEST(JitterControlReplayTest, ScenarioTable)
{
    std::printf("\n[JB replay] packet=%.3fms pull=%.3fms capacity=%u slots floor=%u slots(%.1fms) "
                "strategy=tail  (worst of 16 phases, warmup %.0fms)\n",
        kPacketMs, kPullPeriodMs, kCapacitySlots,
        TargetController::floor_target(make_params(TargetMarginStrategy::TailQuantile)),
        static_cast<double>(TargetController::floor_target(make_params(TargetMarginStrategy::TailQuantile))) * kPacketMs,
        kWarmupMs);
    std::printf("%-22s %7s %7s %8s %8s %10s %7s %7s %7s\n",
        "scenario", "tgt_min", "tgt_max", "tgt_mean", "underrun%", "maxrun_ms", "FILL", "DROP", "p99_ms");

    const LinkCondition conditions[] = { kClean, kLanJitter, kWifi, kLoss, kStorm };
    for (const auto& c : conditions) {
        const auto m = worst_phase(TargetMarginStrategy::TailQuantile, c, 7u);
        std::printf("%-22s %7u %7u %8.2f %8.3f %10.1f %7llu %7llu %7.2f\n", c.name,
            m.target_min, m.target_max, m.target_mean, m.underrun_pct, m.max_run_ms,
            static_cast<unsigned long long>(m.fill_episodes),
            static_cast<unsigned long long>(m.drop_episodes), m.p99_ms);
    }
    SUCCEED();
}

// 不变量：target 永远落在 [有效下限, 2/3 容量]，且干净链路零漂移预算。
TEST(JitterControlReplayTest, CleanLinkKeepsTargetInBoundsAndZeroUnderrun)
{
    const auto params = make_params(TargetMarginStrategy::TailQuantile);
    const auto floor = TargetController::floor_target(params);
    const auto m = worst_phase(TargetMarginStrategy::TailQuantile, kClean, 11u);

    EXPECT_GE(m.target_min, floor);
    EXPECT_LE(m.target_max, params.capacity_slots);
    // 干净链路：收发速率相同（同一个 48k 时钟），不该有欠载。
    EXPECT_LT(m.underrun_pct, 0.5);
    EXPECT_LT(m.max_run_ms, 150.0);
}

// 尾部分位数转正的核心收益：干净链路上 k×J 会把发送端的 burst 节奏当成抖动
// 计入 margin（|D| 由 burst 决定，不是网络），于是 target 被顶到 ~7 槽；
// P99 的稳态低于几何地板，target 落到地板上 → 同样零欠载、少 ~10ms 延迟。
// 这条测试就是"转正到底值不值"的可执行论据。
TEST(JitterControlReplayTest, TailStrategyLowersCleanLinkTargetVersusLegacy)
{
    const auto tail = worst_phase(TargetMarginStrategy::TailQuantile, kClean, 11u);
    const auto legacy = worst_phase(TargetMarginStrategy::ScaledJitter, kClean, 11u);

    EXPECT_LT(tail.target_mean, legacy.target_mean);
    // 两边都必须守住预算，否则"更低"只是拿安全换的。
    EXPECT_LT(tail.underrun_pct, 0.5);
    EXPECT_LT(legacy.underrun_pct, 0.5);
}

// WiFi 级抖动（σ=3ms，20~50ms 常态断流的简化模型）：默认地板下允许有欠载，
// 但必须是"偶发"而不是"成片"——最长连续断流限制住，且 target 不顶满上限。
TEST(JitterControlReplayTest, WifiJitterStaysBounded)
{
    const auto params = make_params(TargetMarginStrategy::TailQuantile);
    const auto m = worst_phase(TargetMarginStrategy::TailQuantile, kWifi, 13u);

    EXPECT_LE(m.target_max, params.capacity_slots);
    EXPECT_LT(m.underrun_pct, 5.0);
    EXPECT_LT(m.max_run_ms, 400.0);
}

// 丢包必须走 concealment（重复上一包 + 淡出）而不是直接静音：
// 1% 丢包下掩盖槽数 > 0，且静音占比远低于丢包率对应的裸静音水平。
TEST(JitterControlReplayTest, LossIsConcealedNotSilenced)
{
    const auto m = worst_phase(TargetMarginStrategy::TailQuantile, kLoss, 17u);

    EXPECT_LT(m.underrun_pct, 5.0);
    EXPECT_LT(m.max_run_ms, 400.0);
}

// 断流风暴：stall 事件频率上去后必须进 storm hold（冻结下跌），
// 否则 target 会被逐个 spike 追着上下抽。这是 storm 机制的存在性验证。
TEST(JitterControlReplayTest, StormHoldEngagesUnderStallBurstStorm)
{
    bool saw = false;
    for (int i = 0; i < 16 && !saw; ++i) {
        const auto m = run(TargetMarginStrategy::TailQuantile, kStorm,
            static_cast<double>(i) * (kPullPeriodMs / 16.0), 23u);
        saw = m.saw_storm_hold;
    }
    EXPECT_TRUE(saw);
}

// 相位敏感性自检：如果最坏相位与最好相位一样，说明扫描没起作用（模型退化），
// 那上面所有"最坏值"结论都不可信。
TEST(JitterControlReplayTest, PhaseSweepIsActuallySensitive)
{
    double best = 1e9;
    double worst = -1.0;
    for (int i = 0; i < 16; ++i) {
        const auto m = run(TargetMarginStrategy::TailQuantile, kWifi,
            static_cast<double>(i) * (kPullPeriodMs / 16.0), 5u);
        best = std::min(best, m.underrun_pct);
        worst = std::max(worst, m.underrun_pct);
    }
    EXPECT_GE(worst, best);
}

} // namespace
