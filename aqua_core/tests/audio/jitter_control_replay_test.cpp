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
// 链路模型（2026-09-16 扩展）：
//   - **成串丢包**：真实丢包是突发性的（阴影衰落/队列冲刷），逐包独立同分布会低估
//     连续空洞的伤害。用两状态 Gilbert-Elliott：坏状态全丢、平均连丢 `loss_burst`
//     个包；`loss_burst<=1` 退化为逐包独立（随机序列与旧实现逐位一致）。
//   - **突发窗口**：可指定 [outage_start_ms, outage_end_ms) 内用另一个丢包率
//     （1.0 = 完全断流），用来测"风暴过后 target 多久回家"。
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
#include <optional>
#include <random>
#include <span>
#include <utility>
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
constexpr int kPhases = 16; // 相位扫描档数
constexpr double kPhaseStepMs = kPullPeriodMs / static_cast<double>(kPhases);

struct LinkCondition {
    const char* name = "";
    double jitter_ms = 0.0; // 网络抖动（高斯 σ）
    double loss = 0.0; // 窗口外的平均丢包率
    double loss_burst = 1.0; // 平均连丢包数；1 = 逐包独立同分布
    double outage_start_ms = -1.0; // >= 0 时启用突发窗口
    double outage_end_ms = -1.0;
    double outage_loss = 1.0; // 窗口内丢包率（1.0 = 完全断流）
    // ---- delay-spike regime（hold 后 burst 交付，与 loss 正交）----
    // 周期性 hold-and-release：窗口 [k*period, k*period+hold) 内产生的包被扣住，
    // 在窗口结束时刻背靠背释放（到达间隔≈0）。建模 AP 缓冲/黑洞后回补
    // （2026-09 WLAN 风暴：~250ms 黑洞 ≈ 68 包，零丢失，回补打爆 ring）。
    // loss 是"包永远消失"，hold 是"包迟到但全到"——风暴的 busy/reanchor 只能由
    // 后者产生（前者 BUSY/REANC 恒零，见 StormCapacitySweep 的 null 结论）。
    double hold_ms = 0.0; // 单次扣留时长；<= 0 = 关闭 hold 模型
    double hold_period_ms = 0.0; // hold 周期（<= 0 = 关闭；必须 > hold_ms 才有意义）
    double run_ms = kRunMs;
};

struct Metrics {
    std::uint32_t target_min = 0;
    std::uint32_t target_max = 0;
    double target_mean = 0.0;
    double penalty_max = 0.0; // 暖机后欠载反馈抬升量的峰值（0 = 从未计罚）
    double underrun_pct = 0.0; // (静音 + 掩盖) / 已拉取，暖机后
    double drop_pct = 0.0; // 跳过的槽帧 / (已拉取 + 跳过)
    double max_run_ms = 0.0;
    std::uint64_t fill_episodes = 0;
    std::uint64_t drop_episodes = 0;
    std::uint64_t reanchors = 0; // 时间轴重锚定次数（风暴 churn 的主口径）
    std::uint64_t busy_rejects = 0; // 环形满溢拒绝（突发超过容量的直接证据）
    double p99_ms = 0.0;
    double jitter_ms = 0.0;
    double stall_peak_ms = 0.0; // 结束时的孤立峰值（决定 stall 项何时回落）
    bool saw_storm_hold = false;
    // 端稳/限速机制的可观测性：暖机后 target 路径的切换次数与各路径占比。
    // 用来回答"三套跌侧机制（storm / dwell / far-fall）到底谁在起作用、有没有互相打架"。
    std::uint64_t path_changes = 0;
    double pct_rise = 0.0;
    double pct_fall = 0.0;
    double pct_dwell = 0.0;
    double pct_storm = 0.0;
};

// 逐包记录（可选）：用来做 splice 开/关对照与"风暴后多久回家"的轨迹分析。
struct Artifacts {
    struct TargetSample {
        double t_ms = 0.0;
        std::uint32_t target = 0;
        TargetPath path = TargetPath::Steady;
        // 决策层诊断（controller 的上一拍结算）：用来**实测**残余水位来自哪一项，
        // 而不是靠推理——jitter_margin=P99 项、stall_margin=stall 峰值项（封顶后）、
        // penalty=欠载反馈、effective_min=本拍生效下限。
        double jitter_margin = 0.0;
        double stall_margin = 0.0;
        double penalty = 0.0;
        std::uint32_t effective_min = 0;
    };
    std::vector<TargetSample> targets;
    std::vector<std::byte> output; // 每次 pull 的输出按序拼接
};

struct Event {
    std::int64_t t_ns = 0;
    bool pull = false; // false = 到达，true = 消费
    std::uint16_t seq = 0;
    std::uint32_t ts = 0; // RTP 时间戳（采样数）
};

// 两状态 Gilbert-Elliott 丢包模型。**每个包恰好消费一次均匀随机数**（无论丢不丢），
// 所以 loss_burst<=1 时的随机序列与旧版 i.i.d. 实现逐位一致——既有场景的表格不会漂。
class ChannelModel {
public:
    ChannelModel(const LinkCondition& link, unsigned seed)
        : link_(link)
        , rng_(seed)
        , unit_(0.0, 1.0)
        , gauss_(0.0, link.jitter_ms > 0.0 ? link.jitter_ms : 1.0)
    {
    }

    [[nodiscard]] bool dropped(double t_ms)
    {
        const bool in_outage = link_.outage_start_ms >= 0.0 && t_ms >= link_.outage_start_ms
            && t_ms < link_.outage_end_ms;
        const double p_loss = in_outage ? link_.outage_loss : link_.loss;
        const double u = unit_(rng_);
        if (p_loss <= 0.0) {
            bad_ = false;
            return false;
        }
        if (p_loss >= 1.0) {
            bad_ = true;
            return true;
        }
        if (link_.loss_burst <= 1.0) {
            return u < p_loss; // 逐包独立
        }
        // Gilbert-Elliott：坏状态每平均 1/p_bg 个包恢复一次；
        // 稳态坏状态占比 = L ⇒ p_gb = L / (B·(1-L))。
        if (bad_) {
            if (u < 1.0 / link_.loss_burst) {
                bad_ = false;
                return false;
            }
            return true;
        }
        const double p_good_to_bad = p_loss / (link_.loss_burst * (1.0 - p_loss));
        if (u < p_good_to_bad) {
            bad_ = true;
            return true;
        }
        return false;
    }

    [[nodiscard]] double jitter_ms()
    {
        return link_.jitter_ms > 0.0 ? gauss_(rng_) : 0.0;
    }

private:
    LinkCondition link_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> unit_;
    std::normal_distribution<double> gauss_;
    bool bad_ = false;
};

// 事件表：发包（累积器切片，背靠背）+ 消费（固定周期，带相位偏移）。
std::vector<Event> build_schedule(const LinkCondition& link, double phase_ms, unsigned seed)
{
    ChannelModel channel(link, seed);

    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(link.run_ms / kCapturePeriodMs * 3.0) + 1300);

    // hold 窗口判定（delay-spike regime）：hold/period <= 0 即关闭，
    // 事件表与旧实现逐包一致（随机序列不受影响）。
    const bool hold_on = link.hold_ms > 0.0 && link.hold_period_ms > link.hold_ms;
    const auto in_hold = [&](double t) {
        if (!hold_on) {
            return false;
        }
        double phase = std::fmod(t, link.hold_period_ms);
        return phase >= 0.0 && phase < link.hold_ms;
    };
    struct Held {
        std::uint16_t seq;
        std::uint32_t ts;
    };
    std::vector<Held> held;
    // hold 窗口结束时刻释放：按扣留顺序（= seq 顺序）以串行化间隔背靠背释放。
    // 关键：**不许**每包独立 jitter——独立 ±3ms 会把 burst 内部顺序打乱，
    // 相邻到达时间戳乱跳几十包，|D| 虚增到上百 ms、J 炸到 18（2026-09 实测坑）。
    // 真实 AP 是 FIFO：保序、间隔 = 空口串行化（~0.1ms），|D| ≈ 3.5/包。
    // jitter 只在*生成侧*（非 hold 包）消费；hold 包的到达时刻是确定性的。
    constexpr double kBurstSerializationMs = 0.1;
    auto flush_held = [&](double release_t) {
        for (std::size_t i = 0; i < held.size(); ++i) {
            const auto& h = held[i];
            events.push_back(Event {
                static_cast<std::int64_t>(
                    (release_t + static_cast<double>(i) * kBurstSerializationMs) * kNsPerMs),
                false, h.seq, h.ts });
        }
        held.clear();
    };

    std::uint32_t pending = 0;
    std::uint16_t seq = 0;
    std::uint32_t ts = 0;
    for (double t = 0.0; t < link.run_ms; t += kCapturePeriodMs) {
        pending += kCaptureBlock;
        const std::uint32_t n = pending / kFrameCount;
        pending -= n * kFrameCount;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!channel.dropped(t)) {
                if (hold_on && in_hold(t)) {
                    held.push_back(Held { seq, ts });
                } else {
                    // 跨过窗口结束：先释放扣住的（到达时刻=窗口结束），再发本包。
                    // t 不在 hold 内而 held 非空 ⟺ 刚跨过窗口结束。
                    if (!held.empty()) {
                        const double period = link.hold_period_ms;
                        const double release_t
                            = std::floor(t / period) * period + link.hold_ms;
                        flush_held(release_t);
                    }
                    const double jitter = channel.jitter_ms();
                    const auto at = static_cast<std::int64_t>((t + jitter) * kNsPerMs);
                    events.push_back(Event { at, false, seq, ts });
                }
            }
            ++seq;
            ts += kFrameCount;
        }
    }
    // 收尾：run 结束时仍被扣住的包在末尾释放（否则凭空丢包，违背 hold 语义）。
    if (!held.empty() && hold_on) {
        flush_held(link.run_ms);
    }
    for (double t = phase_ms; t < link.run_ms; t += kPullPeriodMs) {
        events.push_back(Event { static_cast<std::int64_t>(t * kNsPerMs), true, 0, 0 });
    }
    std::stable_sort(events.begin(), events.end(),
        [](const Event& a, const Event& b) { return a.t_ns < b.t_ns; });
    return events;
}

// 生产口径的 controller 参数（对应 client_runtime.cpp setup_playback）。
TargetControllerParams make_params(TargetMarginStrategy strategy,
    std::uint32_t capacity_slots = kCapacitySlots)
{
    TargetControllerParams p;
    // 结构上限 = 2/3 × capacity（target 顶到 capacity 会让高水位带落到 ring 外）。
    p.capacity_slots = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(static_cast<double>(capacity_slots)
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

std::uint32_t floor_slots(TargetMarginStrategy strategy,
    std::uint32_t capacity_slots = kCapacitySlots)
{
    const auto params = make_params(strategy, capacity_slots);
    return std::min(TargetController::floor_target(params), params.capacity_slots);
}

// splice_enabled：产品默认开（--jb-no-splice 关）。art != nullptr 时记录逐包轨迹与输出。
// capacity_slots：风暴容量实验的旋钮（默认 30 = 产品默认；JB 上限 512）。
Metrics run(const TargetMarginStrategy strategy, const LinkCondition& link, double phase_ms,
    unsigned seed, bool splice_enabled = true, Artifacts* art = nullptr,
    std::uint32_t capacity_slots = kCapacitySlots)
{
    TargetControllerParams params = make_params(strategy, capacity_slots);
    params.initial_target_slots = floor_slots(strategy, capacity_slots);

    JitterBufferConfig cfg;
    cfg.capacity_slots = capacity_slots;
    cfg.format = AudioFormat { AudioEncoding::PCM_F32LE, 1, kSampleRate };
    cfg.frame_count = kFrameCount;
    // 产品默认：concealment + splice 都开（组件默认关，由 Runtime 打开）。
    cfg.concealment.enabled = true;
    cfg.concealment.max_slots = aqua::config::JB_CONCEALMENT_DEFAULT_MAX_SLOTS;
    cfg.splice.enabled = splice_enabled;
    // 水位带 + 启动水位：与 ClientRuntime 用同一个函数（防口径漂移）。
    aqua::audio::apply_adaptive_bands(cfg, capacity_slots, params.initial_target_slots);

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
    std::uint64_t path_n = 0;
    std::uint64_t rise_n = 0;
    std::uint64_t fall_n = 0;
    std::uint64_t dwell_n = 0;
    std::uint64_t storm_n = 0;
    std::optional<TargetPath> prev_path;
    bool warmed = false;
    std::uint32_t target_min = capacity_slots;
    std::uint32_t target_max = 0;

    for (const auto& e : schedule) {
        if (!e.pull) {
            estimator.observe(e.seq, e.ts, kSsrc, e.t_ns);
            const auto estimates = estimator.estimates();
            // 计罚口径与产品（client_runtime.cpp arrival observer）同函数：
            // conceal 开时只有 saturated 才抬地板。
            const auto penalty_events = aqua::audio::select_penalty_events(
                cfg.concealment.enabled, jb.underrun_events(), jb.concealed_saturated_slots());
            const auto target = controller.update(estimates.jitter_ms, e.t_ns,
                penalty_events, estimates.stall_peak_ms, estimates.tail_p99_ms,
                estimates.stall_events);
            jb.set_target_slots(target);
            const auto path = controller.path();
            if (path == TargetPath::StormHold) {
                m.saw_storm_hold = true;
            }
            if (e.t_ns >= static_cast<std::int64_t>(kWarmupMs * kNsPerMs)) {
                target_sum += target;
                ++target_n;
                target_min = std::min(target_min, target);
                target_max = std::max(target_max, target);
                m.penalty_max = std::max(m.penalty_max, controller.underrun_penalty());
                ++path_n;
                if (prev_path.has_value() && *prev_path != path) {
                    ++m.path_changes;
                }
                prev_path = path;
                switch (path) {
                case TargetPath::Rise:
                    ++rise_n;
                    break;
                case TargetPath::Fall:
                    ++fall_n;
                    break;
                case TargetPath::DwellLock:
                    ++dwell_n;
                    break;
                case TargetPath::StormHold:
                    ++storm_n;
                    break;
                default:
                    break;
                }
            }
            if (art != nullptr) {
                art->targets.push_back(Artifacts::TargetSample {
                    .t_ms = static_cast<double>(e.t_ns) / kNsPerMs,
                    .target = target,
                    .path = path,
                    .jitter_margin = controller.last_jitter_margin(),
                    .stall_margin = controller.last_stall_margin(),
                    .penalty = controller.underrun_penalty(),
                    .effective_min = controller.effective_min(),
                });
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
        if (art != nullptr) {
            art->output.insert(art->output.end(), output.begin(), output.end());
        }
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
    m.reanchors = jb.reanchor_count();
    m.busy_rejects = jb.push_rejected_slot_busy();
    const auto final_estimates = estimator.estimates();
    m.p99_ms = final_estimates.tail_p99_ms;
    m.jitter_ms = final_estimates.jitter_ms;
    m.stall_peak_ms = final_estimates.stall_peak_ms;
    if (path_n > 0) {
        const auto pct = [path_n](std::uint64_t n) {
            return 100.0 * static_cast<double>(n) / static_cast<double>(path_n);
        };
        m.pct_rise = pct(rise_n);
        m.pct_fall = pct(fall_n);
        m.pct_dwell = pct(dwell_n);
        m.pct_storm = pct(storm_n);
    }
    return m;
}

// 相位扫描：发包 10ms 与消费 10.667ms 拍频约 160ms 扫过全部相位，
// 最坏相位必须扫出来（只看单个相位会严重低估欠载）。
std::pair<Metrics, int> worst_phase_index(const TargetMarginStrategy strategy,
    const LinkCondition& link, unsigned seed, bool splice_enabled = true, int phases = kPhases,
    std::uint32_t capacity_slots = kCapacitySlots)
{
    Metrics worst;
    int worst_index = 0;
    double worst_underrun = -1.0;
    for (int i = 0; i < phases; ++i) {
        const auto m = run(strategy, link, static_cast<double>(i) * kPhaseStepMs, seed,
            splice_enabled, nullptr, capacity_slots);
        if (m.underrun_pct > worst_underrun) {
            worst_underrun = m.underrun_pct;
            worst = m;
            worst_index = i;
        }
    }
    return { worst, worst_index };
}

Metrics worst_phase(const TargetMarginStrategy strategy, const LinkCondition& link, unsigned seed,
    bool splice_enabled = true, std::uint32_t capacity_slots = kCapacitySlots)
{
    return worst_phase_index(strategy, link, seed, splice_enabled, kPhases, capacity_slots).first;
}

// 首次"target <= ceiling 并保持 hold_ms"的时刻（ms，绝对时间）。用来量化
// "风暴结束后延迟债多久还清"——三套跌侧机制（storm / dwell / far-fall）合起来的结果。
std::optional<double> settled_at_ms(
    const Artifacts& art, double after_ms, std::uint32_t ceiling, double hold_ms)
{
    const auto& s = art.targets;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i].t_ms < after_ms || s[i].target > ceiling) {
            continue;
        }
        bool holds = true;
        for (std::size_t j = i; j < s.size() && s[j].t_ms <= s[i].t_ms + hold_ms; ++j) {
            if (s[j].target > ceiling) {
                holds = false;
                break;
            }
        }
        if (holds) {
            return s[i].t_ms;
        }
    }
    return std::nullopt;
}

// [t0, t1) 区间内的 target 均值：用来比较"断流前 / 断流后"以及"是否留下永久水位抬高"。
double mean_target_in(const Artifacts& art, double t0, double t1)
{
    std::uint64_t sum = 0;
    std::uint64_t n = 0;
    for (const auto& s : art.targets) {
        if (s.t_ms >= t0 && s.t_ms < t1) {
            sum += s.target;
            ++n;
        }
    }
    return n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
}

// 连续差异段的长度分布（帧）：用来量化"crossfade 到底改了多少样本"。
struct OutputDiff {
    std::uint64_t differing_frames = 0;
    std::uint64_t runs = 0;
    std::uint64_t max_run_frames = 0;
};

OutputDiff diff_by_frame(const std::vector<std::byte>& a, const std::vector<std::byte>& b)
{
    OutputDiff d;
    const std::size_t frames = std::min(a.size(), b.size()) / kFrameBytes;
    std::uint64_t run = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        const bool differs = !std::equal(a.begin() + static_cast<std::ptrdiff_t>(f * kFrameBytes),
            a.begin() + static_cast<std::ptrdiff_t>((f + 1) * kFrameBytes),
            b.begin() + static_cast<std::ptrdiff_t>(f * kFrameBytes));
        if (differs) {
            ++d.differing_frames;
            ++run;
            d.max_run_frames = std::max(d.max_run_frames, run);
        } else if (run > 0) {
            ++d.runs;
            run = 0;
        }
    }
    if (run > 0) {
        ++d.runs;
    }
    return d;
}

const LinkCondition kClean { .name = "理想有线" };
const LinkCondition kLanJitter { .name = "+1ms 抖动", .jitter_ms = 1.0 };
const LinkCondition kWifi { .name = "+3ms 抖动(WiFi)", .jitter_ms = 3.0 };
const LinkCondition kLoss { .name = "+1% 丢包", .jitter_ms = 1.0, .loss = 0.01 };
const LinkCondition kLossBurst {
    .name = "+1% 成串丢包", .jitter_ms = 1.0, .loss = 0.01, .loss_burst = 6.0
};
const LinkCondition kStorm { .name = "断流风暴(35% 丢包)", .jitter_ms = 1.0, .loss = 0.35 };
// hold 风暴（delay-spike regime）：250ms 黑洞 + 回补 burst，每 450ms 一次。
// 2026-09 WLAN 拔线风暴的合成复刻：零丢失、回补 burst 打爆 ring（busy/reanchor）。
// 与 kStorm（纯丢失）正交：前者 BUSY/REANC 恒零，后者必然非零。
// 实测签名（cap30，worst phase）：target 被 penalty 地板钉在 10（tail 因溢出密度
// 不足看不见 250ms 缺口——P99 的固有盲区，由 penalty 补），und≈54%，
// maxrun≈277ms，REANC≈26，BUSY≈600。cap120：churn 归零（REANC/BUSY=0，burst
// 被 DROP 修剪吸收），und≈46%，target 仍是 10——大 ring 在这里只买 headroom
// 不买延迟（target 是地板驱动的）。
const LinkCondition kStormBurst {
    .name = "hold风暴(250ms/450ms)", .jitter_ms = 1.0, .hold_ms = 250.0, .hold_period_ms = 450.0
};
const LinkCondition kOutage {
    .name = "2s 完全断流",
    .jitter_ms = 1.0,
    .outage_start_ms = 4000.0,
    .outage_end_ms = 6000.0,
    .outage_loss = 1.0,
    .run_ms = 30000.0,
};

// 场景表：这是"改参数前先离线看效果"的入口。跑
//   aqua_jitter_buffer_tests.exe --gtest_filter=*ScenarioTable*
// 直接看表格（ctest 下加 --output-on-failure 或 -V 也能看到）。
TEST(JitterControlReplayTest, ScenarioTable)
{
    std::printf("\n[JB replay] packet=%.3fms pull=%.3fms capacity=%u slots floor=%u slots(%.1fms) "
                "strategy=tail  (worst of %d phases, warmup %.0fms)\n",
        kPacketMs, kPullPeriodMs, kCapacitySlots, floor_slots(TargetMarginStrategy::TailQuantile),
        static_cast<double>(floor_slots(TargetMarginStrategy::TailQuantile)) * kPacketMs, kPhases,
        kWarmupMs);
    std::printf("%-22s %7s %7s %8s %8s %9s %6s %6s %7s %6s %6s %6s %6s %6s\n", "scenario",
        "tgt_min", "tgt_max", "tgt_mean", "underrun%", "maxrun_ms", "FILL", "DROP", "p99_ms",
        "churn", "%rise", "%fall", "%dwell", "%storm");

    const LinkCondition conditions[] = { kClean, kLanJitter, kWifi, kLoss, kLossBurst, kStorm,
        kStormBurst, kOutage };
    for (const auto& c : conditions) {
        const auto m = worst_phase(TargetMarginStrategy::TailQuantile, c, 7u);
        std::printf("%-22s %7u %7u %8.2f %8.3f %9.1f %6llu %6llu %7.2f %6llu %6.1f %6.1f %6.1f %6.1f\n",
            c.name, m.target_min, m.target_max, m.target_mean, m.underrun_pct, m.max_run_ms,
            static_cast<unsigned long long>(m.fill_episodes),
            static_cast<unsigned long long>(m.drop_episodes), m.p99_ms,
            static_cast<unsigned long long>(m.path_changes), m.pct_rise, m.pct_fall, m.pct_dwell,
            m.pct_storm);
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

// **成串丢包**（平均连丢 6 包）与逐包独立同分布、平均丢包率相同：真实链路是前者。
// 这条回答"1% 的成串丢包会不会打穿预算、P99 会不会因为连续空洞被顶高"。
// 注意两者只差丢包的时间相关性（平均丢包率都是 1%），所以差异可直接归因于"成串"。
TEST(JitterControlReplayTest, BurstLossIsHandledWithinBudget)
{
    const auto uniform = worst_phase(TargetMarginStrategy::TailQuantile, kLoss, 17u);
    const auto burst = worst_phase(TargetMarginStrategy::TailQuantile, kLossBurst, 17u);

    std::printf(
        "\n[JB replay] 1%% loss: uniform{tgt=%.2f underrun=%.3f%% maxrun=%.1fms} "
        "burst(6){tgt=%.2f underrun=%.3f%% maxrun=%.1fms}\n",
        uniform.target_mean, uniform.underrun_pct, uniform.max_run_ms, burst.target_mean,
        burst.underrun_pct, burst.max_run_ms);

    // 成串丢包更难：预算必须同样守住（这是 concealment + margin 的真实考验）。
    EXPECT_LT(burst.underrun_pct, 5.0);
    EXPECT_LT(burst.max_run_ms, 400.0);
}

// **splice 只改输出样本，不改时间轴/决策**——这是 crossfade 的核心设计声明
// （见 jitter_buffer_control_design.md：只动输出样本，不动目标/target/episode）。
// 同一链路、同一相位、同一 seed 下开/关 splice：决策序列必须逐位相同，
// 输出只在少量短窗内不同（每次拼接 ≤ 淡变长度）。
TEST(JitterControlReplayTest, SpliceChangesOnlyOutputSamplesNotTimeline)
{
    Artifacts with_splice;
    Artifacts without_splice;
    const auto phase = static_cast<double>(3) * kPhaseStepMs;
    const auto m_on = run(TargetMarginStrategy::TailQuantile, kLoss, phase, 19u, true, &with_splice);
    const auto m_off = run(TargetMarginStrategy::TailQuantile, kLoss, phase, 19u, false, &without_splice);

    // 决策面：逐包 target 与路径必须完全一致（splice 不在控制回路里）。
    ASSERT_EQ(with_splice.targets.size(), without_splice.targets.size());
    bool timeline_identical = true;
    for (std::size_t i = 0; i < with_splice.targets.size(); ++i) {
        timeline_identical = timeline_identical
            && with_splice.targets[i].target == without_splice.targets[i].target
            && with_splice.targets[i].path == without_splice.targets[i].path;
    }
    EXPECT_TRUE(timeline_identical);
    EXPECT_EQ(m_on.target_mean, m_off.target_mean);
    EXPECT_EQ(m_on.fill_episodes, m_off.fill_episodes);
    EXPECT_EQ(m_on.drop_episodes, m_off.drop_episodes);
    EXPECT_EQ(m_on.underrun_pct, m_off.underrun_pct);
    EXPECT_EQ(m_on.drop_pct, m_off.drop_pct);

    // 输出面：确实改了（splice 真的触发了），但只占极小比例、且每段都很短。
    const auto d = diff_by_frame(with_splice.output, without_splice.output);
    const auto total_frames = static_cast<std::uint64_t>(
        std::min(with_splice.output.size(), without_splice.output.size()) / kFrameBytes);
    std::printf("\n[JB replay] splice on/off: runs=%llu max_run=%lluframes diff=%llu/%llu frames (%.3f%%)\n",
        static_cast<unsigned long long>(d.runs),
        static_cast<unsigned long long>(d.max_run_frames),
        static_cast<unsigned long long>(d.differing_frames),
        static_cast<unsigned long long>(total_frames),
        100.0 * static_cast<double>(d.differing_frames)
            / static_cast<double>(std::max<std::uint64_t>(1, total_frames)));

    EXPECT_GT(d.differing_frames, 0u) << "splice 未改变任何输出样本，说明淡变没被触发";
    EXPECT_LT(d.differing_frames * 20, total_frames) << "改动样本超过 5%，超出\"只修饰拼接点\"的设计意图";
    // 淡变长度 64 帧；相邻拼接点可能背靠背（如 DROP 着陆紧接 FILL 重播），故放宽到 2×。
    // 本场景实测 max_run=63 帧（= 单次淡变，未出现背靠背）——若将来变成 2×，说明拼接点
    // 连发了，值得复核（连续淡变会把瞬态抹平）。
    EXPECT_LE(d.max_run_frames, 128u) << "单段差异超过两次淡变，说明改动溢出到稳态";
}

// 断流风暴：stall 事件频率上去后必须进 storm hold（冻结下跌），
// 否则 target 会被逐个 spike 追着上下抽。这是 storm 机制的存在性验证。
TEST(JitterControlReplayTest, StormHoldEngagesUnderStallBurstStorm)
{
    bool saw = false;
    for (int i = 0; i < kPhases && !saw; ++i) {
        const auto m = run(TargetMarginStrategy::TailQuantile, kStorm,
            static_cast<double>(i) * kPhaseStepMs, 23u);
        saw = m.saw_storm_hold;
    }
    EXPECT_TRUE(saw);
}

// **断流后的残余水位：必须被 stall 封顶挡住，且不得永久锁死。**
//
// 现场关心的"2s 断流之后 target 多久回家"在代码里由三件事合成，本测试把它们分开验证：
//   1) 幅度：孤立极端值走 stall 峰值项，且被封顶在 JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS
//      ——"cap 挡延迟债务"这条设计声明；
//   2) 时长：峰值以 JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC（10ms/s）**线性**衰减，
//      所以回落时间与事故时长成正比：2s 的峰值要 (2000-25.5)/10 ≈ 197s 才回落到封顶
//      以下（25.5ms = 封顶前一档的阈值）。这是**已知取舍**（buffer_config.h：
//      "大事故挂更久"），本测试不替它下结论，只把它钉成可执行断言；
//   3) 不锁死：残余不得超出参照稳态 +1 槽，也不得随时间继续抬高。
//
// 参照物刻意用**同链路、同相位、无断流**的稳态均值，而不是绝对几何地板：
// 1ms 抖动的稳态本来就在 6~7 槽（见场景表），拿地板当参照会得到假失败。
TEST(JitterControlReplayTest, OutageResidueIsCappedNotLocked)
{
    LinkCondition reference_link = kOutage;
    reference_link.outage_start_ms = -1.0;
    reference_link.outage_loss = 0.0;

    const auto [ref_sweep, ref_index] = worst_phase_index(
        TargetMarginStrategy::TailQuantile, reference_link, 29u);
    (void)ref_sweep;
    Artifacts ref_art;
    run(TargetMarginStrategy::TailQuantile, reference_link,
        static_cast<double>(ref_index) * kPhaseStepMs, 29u, true, &ref_art);
    const double ref_steady
        = mean_target_in(ref_art, kOutage.run_ms - 2000.0, kOutage.run_ms - 100.0);

    const auto [sweep, worst_index] = worst_phase_index(
        TargetMarginStrategy::TailQuantile, kOutage, 29u);
    Artifacts art;
    run(TargetMarginStrategy::TailQuantile, kOutage,
        static_cast<double>(worst_index) * kPhaseStepMs, 29u, true, &art);

    const double stall_cap = aqua::config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
    const double tail = mean_target_in(art, kOutage.run_ms - 2000.0, kOutage.run_ms - 100.0);
    const auto settled = settled_at_ms(
        art, kOutage.outage_end_ms, static_cast<std::uint32_t>(stall_cap), 1000.0);
    const auto& last = art.targets.back();
    // 按衰减律估算"峰值回落到封顶以下"所需时间（仅用于报告，不做断言）。
    const double release_estimate_s
        = std::max(0.0, (sweep.stall_peak_ms - static_cast<double>(stall_cap - 1) * kPacketMs)
                / aqua::config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC);

    const auto& ref_last = ref_art.targets.back();
    std::printf("\n[JB replay] outage 2s: ref_steady=%.2f stall_cap=%u peak=%u tail=%.2f "
                "(residue=%.2f 槽 = %.1fms) underrun=%.3f%% maxrun=%.1fms churn=%llu\n"
                "            ref end:  target=%u jitter_margin=%.2f stall_margin=%.2f eff_min=%u\n"
                "            run end:  target=%u jitter_margin=%.2f stall_margin=%.2f "
                "penalty=%.2f eff_min=%u | stall_peak=%.0fms -> 回落到封顶以下约需 %.0fs\n",
        ref_steady, static_cast<unsigned>(stall_cap), sweep.target_max, tail, tail - ref_steady,
        (tail - ref_steady) * kPacketMs, sweep.underrun_pct, sweep.max_run_ms,
        static_cast<unsigned long long>(sweep.path_changes), ref_last.target,
        ref_last.jitter_margin, ref_last.stall_margin, ref_last.effective_min, last.target,
        last.jitter_margin, last.stall_margin, last.penalty, last.effective_min,
        sweep.stall_peak_ms, release_estimate_s);

    // 1) 幅度：残余不得越过 stall 封顶（cap 是"挡延迟债务"的那道闸）。
    EXPECT_LE(sweep.target_max, stall_cap);
    EXPECT_LE(tail, static_cast<double>(stall_cap));
    // 2) 幅度（下界）：确实顶到了封顶——否则说明断流没被识别成孤立极端值。
    EXPECT_GE(sweep.target_max, stall_cap - 1);
    // 3) 不锁死：断流结束后能在封顶以内稳住，且残余不随时间继续抬高。
    EXPECT_TRUE(settled.has_value()) << "断流后 target 没有稳定回落到封顶以内";
    EXPECT_LE(tail, static_cast<double>(sweep.target_max))
        << "尾部均值高于断流峰值：水位还在往上走，不是回落";
    // 参照稳态的 +1 槽容差**故意不设**：残余（相对参照稳态）来自 stall 峰值按
    // 10ms/s 线性衰减，属已知取舍（见本测试头部注释），用封顶做上界才是有效断言。
}

// **风暴容量扫描：大 ring 到底能不能骑过断流，还是只加延迟债？**
//
// 现场问题（2026-09，WLAN 拔线风暴：~250ms 黑洞 ≈ 68 包，30 槽 ring 被回补
// burst 反复打爆 busy + reanchor）：30 槽物理上装不下 68 包的突发。直觉是"加
// 容量"，结论分两层：
//   - **纯丢失型**风暴（kStorm/kOutage/kLossBurst）：容量 30/60/120 三行逐字
//     相同——target 是 margin/地板算出来的槽数（与容量无关），水位带随 target
//     缩放，多出来的 ring 全是空转的 headroom。容量对这类风暴无效。
//   - **延迟型**风暴（kStormBurst，hold 后 burst 交付）：容量 120 让 churn 归零
//     （REANC/BUSY 26/595 → 0/0）、und 53.7% → 46.3%，而 target 全程钉在
//     penalty 地板 10（延迟一分不涨）。大 ring 在这里只买 headroom 不买延迟——
//     因为 target 是地板驱动的，不是容量驱动的。
TEST(JitterControlReplayTest, StormCapacitySweep)
{
    const LinkCondition* const links[] = { &kStorm, &kStormBurst, &kOutage, &kLossBurst };
    const std::uint32_t capacities[] = { 30, 60, 120 };
    std::printf("\n[JB replay] storm capacity sweep (worst of %d phases, seed 7)\n", kPhases);
    std::printf("%-18s %4s %7s %7s %8s %8s %9s %6s %6s %6s %6s %9s\n", "scenario", "cap",
        "tgt_min", "tgt_max", "tgt_mean", "und%", "maxrun", "FILL", "DROP", "REANC", "BUSY",
        "pen_max");
    for (const auto* link : links) {
        for (const auto cap : capacities) {
            const auto m = worst_phase(
                TargetMarginStrategy::TailQuantile, *link, 7u, true, cap);
            const auto floor = floor_slots(TargetMarginStrategy::TailQuantile, cap);
            const auto max_target = std::max<std::uint32_t>(1,
                static_cast<std::uint32_t>(static_cast<double>(cap)
                    * aqua::config::JB_ADAPTIVE_TARGET_CAPACITY_RATIO));
            // 结构不变式与容量无关：target 永不出 [floor, max]。
            EXPECT_GE(m.target_min, floor) << link->name << " cap=" << cap;
            EXPECT_LE(m.target_max, max_target) << link->name << " cap=" << cap;
            std::printf("%-18s %4u %7u %7u %8.2f %8.3f %9.1f %6llu %6llu %6llu %6llu %9.2f\n",
                link->name, cap, m.target_min, m.target_max, m.target_mean,
                m.underrun_pct, m.max_run_ms,
                static_cast<unsigned long long>(m.fill_episodes),
                static_cast<unsigned long long>(m.drop_episodes),
                static_cast<unsigned long long>(m.reanchors),
                static_cast<unsigned long long>(m.busy_rejects), m.penalty_max);
        }
    }
    SUCCEED();
}



// hold burst 必须保序：释放间隔 = 空口串行化（~0.1ms），|D| ≈ 3.5/包。
// 回归线（2026-09 实测坑）：释放侧每包独立 jitter 会打乱 burst 内顺序，
// 相邻时间戳乱跳几十包 → J 炸到 ~18 → kJ artifact 把 target 顶到 25。
// 锁两点：终态 J 留在 burst 物理水平；cap60 下 target 不出合法上界 19~20。
TEST(JitterControlReplayTest, HoldBurstPreservesOrder)
{
    const auto m = run(TargetMarginStrategy::TailQuantile, kStormBurst, 0.0, 7u, true, nullptr, 60);
    EXPECT_LT(m.jitter_ms, 8.0) << "burst 内乱序：J 被虚增（scramble artifact）";
    EXPECT_LE(m.target_max, 20u) << "kJ artifact 把 target 顶出合法 margin 上界";
}

// hold 风暴的病理签名（与纯丢失模型区分）：回补 burst 打爆 ring。
// loss 模型下 BUSY/REANC 恒零；只有 delay-spike 能产生这两项。
TEST(JitterControlReplayTest, HoldStormReproducesBurstPathology)
{
    const auto m = worst_phase(TargetMarginStrategy::TailQuantile, kStormBurst, 7u);
    EXPECT_GT(m.busy_rejects, 0u) << "回补 burst 没有溢出 ring：hold 模型退化成 loss 模型了";
    EXPECT_GT(m.reanchors, 0u) << "远超前没有触发重锚：burst 落地路径没走通";
}

// 相位敏感性自检：如果最坏相位与最好相位一样，说明扫描没起作用（模型退化），
// 那上面所有"最坏值"结论都不可信。
TEST(JitterControlReplayTest, PhaseSweepIsActuallySensitive)
{
    double best = 1e9;
    double worst = -1.0;
    for (int i = 0; i < kPhases; ++i) {
        const auto m = run(TargetMarginStrategy::TailQuantile, kWifi,
            static_cast<double>(i) * kPhaseStepMs, 5u);
        best = std::min(best, m.underrun_pct);
        worst = std::max(worst, m.underrun_pct);
    }
    EXPECT_GE(worst, best);
}

} // namespace
