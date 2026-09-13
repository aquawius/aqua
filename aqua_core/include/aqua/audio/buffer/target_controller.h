#ifndef AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
#define AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H

// Phase 1 自适应延迟改造：只负责计算 target，不执行播放。
//
// 职责边界（见 JB 自适应改造细则 §1/§3）：
//   JitterEstimator = 只负责观察网络
//   TargetController = 只负责计算 target_slots
//   JitterBuffer = 继续负责实际播放与 Fill/Drop/reanchor
//
// 控制律：target = clamp(margin, effective_min, max)。
// margin = max(k×J, min(stall 峰值/包周期 + 余量, 上限))——k×J 用均值型观测
// 覆盖常态抖动，stall 峰值（NetEq 式 peak detection）补被 stall 门剔除的
// 拥塞尾部，cap（config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS）把"孤立大 stall"
// 挡在 steady-state 延迟债务之外。effective_min = 几何地板/min_target +
// 欠载惩罚（feedback 只抬下限不叠 margin，k×J 高时不重复放大）。
// target 的物理意义 = "JB 该持有多少已到达的数据"；网络底噪/单程延迟
// （estimator 的 base_delay）不是 buffer budget，已移出公式（诊断保留）。
// margin 策略必须可替换（percentile/histogram/peak/hybrid 留给未来）：
// MarginStrategy 枚举 + compute_margin() 分支就是扩展点，不要把公式焊死。
//
// 控制迟滞（细则 §4）：涨快跌慢 + 死区。恶化立即跟进，恢复按 fall_rate
// 限速，deadband 内变化忽略，杜绝 target 来回抽动。
//
// 约束：无 IO、无锁、无分配、O(1) 每次 update；只在 push strand 调用。
// 例外：AQUA_JB_CONTROL_THREAD_DEBUG_LOG 开启时 update() 会同步打一行决策日志
// （事件驱动 + 稳态节流）。这是**开发期开关**，默认关；开启后本组件不再满足
// "无 IO"——与 RT 侧 AQUA_JB_RUNTIME_THREAD_DEBUG_LOG 是同一取舍。
//
// 默认取值集中在 aqua/audio/buffer/buffer_config.h（namespace aqua::config，
// 前缀 JB_ADAPTIVE_*）：改默认值改那里，本文件只保留结构与语义说明。

#include "aqua/audio/buffer/buffer_config.h"

#include <atomic>
#include <cstdint>

namespace aqua::audio {

// margin 策略扩展点（Phase 1 只有 ScaledJitter；加策略 = 加枚举 + 分支）。
enum class TargetMarginStrategy : std::uint8_t { ScaledJitter = 0 };

// 最近一次 update 中 margin 的胜出方（诊断用）：target 为什么变必须可解释，
// 否则只能从 jit/stall_peak/penalty 倒推。
enum class TargetMarginSource : std::uint8_t { Jitter = 0,
    StallPeak = 1 };

[[nodiscard]] inline const char* target_margin_source_name(TargetMarginSource s) noexcept
{
    return s == TargetMarginSource::StallPeak ? "stall_peak" : "kJ";
}

// 本拍 current 的收敛路径（诊断用）。为什么需要：只打"target 变化"时，
// "desired 真的等于 current"与"被涨后 dwell / 跌侧限速按住了"在日志里长得
// 一模一样——而这两件事的处置完全不同（前者健康，后者要查 dwell/fall_rate）。
enum class TargetPath : std::uint8_t {
    Steady = 0, // desired == current，无动作
    Rise = 1, // 恶化：超死区立即跟进到 desired（涨快）
    Fall = 2, // 恢复：按 fall_rate 限速下跌（可能一拍只降一部分）
    DwellLock = 3, // 涨后 dwell 窗口内锁跌（峰值保持）
    Deadband = 4, // desired 高于 current 但差值 ≤ deadband，被死区吞掉
    NoTimeBase = 5, // 首拍 / 时钟不前进：跌侧全额跟进（无时间基可限速）
};

[[nodiscard]] inline const char* target_path_name(TargetPath p) noexcept
{
    switch (p) {
    case TargetPath::Steady:
        return "steady";
    case TargetPath::Rise:
        return "rise";
    case TargetPath::Fall:
        return "fall";
    case TargetPath::DwellLock:
        return "dwell_lock";
    case TargetPath::Deadband:
        return "deadband";
    case TargetPath::NoTimeBase:
        return "no_time_base";
    }
    return "unknown";
}

struct TargetControllerParams {
    // 上限来源：target 永不超过 capacity。ClientRuntime 传的是 2/3 × capacity
    // （结构上限，理由见 config::JB_ADAPTIVE_TARGET_CAPACITY_RATIO）而不是
    // capacity 本身——整条高水位带必须留在 ring 之内，DROP 才有权威。
    std::uint32_t capacity_slots = config::JB_DEFAULT_CAPACITY_SLOTS;
    double packet_ms = config::JB_ADAPTIVE_DEFAULT_PACKET_MS; // 包时长 = F×1000/sample_rate（ms）
    // target 硬下限。**有效下限 = max(本值, 几何地板 + 1)**——几何地板无条件
    // 托底，本值只能抬高、不能压低。默认 3，取值理由见
    // config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS。
    std::uint32_t min_target_slots = config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS;
    std::uint32_t initial_target_slots
        = config::JB_ADAPTIVE_DEFAULT_INITIAL_TARGET_SLOTS; // 起步：快启与安全的折中
    // 几何地板：一次 playback callback 要消耗 ceil(callback_frames / F) 个包。
    // target ≤ 该值意味着"每个 callback 都必然把 JB 抽空"——这不是抖动问题，
    // 是结构性不可能（F=175@48k + 480 帧 callback → floor=4）。target 必须
    // 至少 floor+1：一个 callback 的口粮 + 一包余量垫住到达相位。
    // 0 = 调用方未提供（组件单独使用 / 单测），此时退化为 min_target_slots。
    std::uint32_t geometric_floor_slots = 0;
    // k：margin = k×J（包单位）。**自适应模式的主力旋钮**。取值理由（为什么
    // 不是教科书的 2~3）见 config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN。
    double jitter_gain = config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN;
    // 恢复限速：每秒最多降这么多。只锁**下跌**——上涨永远即时。
    double fall_rate_slots_per_sec = config::JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC;
    // 上涨后的峰值保持窗口（ms）：窗口内不允许下跌。为什么需要（J 在 ceil
    // 边界摆动导致 target 7↔8 抽动）见 config::JB_ADAPTIVE_RISE_DWELL_MS。
    double rise_dwell_ms = config::JB_ADAPTIVE_RISE_DWELL_MS;
    // ---- 欠载反馈（细则 §3：underrun history 是 controller 的输入）----
    // 预测项 k×J 用的是均值，覆盖不了随机抖动尾部与丢包；反馈项补这个洞：
    // 发生欠载就把 target 的**下限**顶上去，一段时间不再欠载再慢慢放下。
    // 它是安全网不是主力，干净链路上恒为 0。取值理由见
    // config::JB_ADAPTIVE_UNDERRUN_PENALTY_*（buffer_config.h）。
    double underrun_penalty_per_event
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS; // 每次欠载事件抬升的下限（槽）。
    // 0 = 关闭整条反馈闭环（合法极值，对照实验用）；负值/非有限 = 退回默认。
    std::uint32_t underrun_penalty_max_slots
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS; // 反馈项累计上限（防病态放大）
    double underrun_penalty_decay_slots_per_sec
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC; // 无新欠载时的回落速率
    // 死区：期望与当前差值在该范围内不动。**默认 0**，且不建议改 —— 死区与
    // "跌侧不限死区 grind 到底"叠加会产生永久偏移。理由见
    // config::JB_ADAPTIVE_DEADBAND_SLOTS（buffer_config.h）。
    std::uint32_t deadband_slots = config::JB_ADAPTIVE_DEADBAND_SLOTS;
    // stall 峰值项的上限（槽）= `--jb-stall-peak-cap`。stall 是"已经发生的恢复
    // 风险信号"，不是新的 steady-state 延迟要求；本值决定一次孤立大 stall 最多
    // 把 target 推多高（超过的部分交给欠载惩罚 + concealment + reanchor 各管一段）。
    // 取值理由见 config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS。**0 = 关闭 stall 峰值
    // 项**（margin 退回纯 k×J），可用于分离"预测项"的贡献；负值 = 默认值。
    double stall_peak_cap_slots = config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
    TargetMarginStrategy margin_strategy = TargetMarginStrategy::ScaledJitter;
};

class TargetController {
public:
    // 给定参数算出 target 的硬下限 = max(min_target_slots, 几何地板 + 1)。
    // 语义见 TargetControllerParams::min_target_slots。
    // 构造前即可调用（ClientRuntime 需要它决定起步水位，而那时 controller 还没建），
    // 构造与运行期共用同一份口径——地板逻辑只此一处，不要在两个地方各算一遍。
    [[nodiscard]] static std::uint32_t floor_target(const TargetControllerParams& params) noexcept;

    // 运行期校正几何地板（playback 实际 callback 帧数与请求值不同时）。
    // 语义与构造期同一口径：新下限 = max(min_target_slots 参数, floor+1) 再夹
    // 容量；floor=0 = 撤销地板（退回 min_target_slots）。
    // 线程安全：内部只写 min_target_ 原子（控制线程调用，push strand 的
    // update() relaxed 读）。current_ 不动——若新下限高于 current_，下一次
    // update() 的 desired clamp 会经"涨快"分支立即拉起；低于则走跌侧限速
    // 自然回落，不会跳变。
    void update_geometric_floor(std::uint32_t geometric_floor_slots) noexcept;

    explicit TargetController(const TargetControllerParams& params) noexcept;

    TargetController(const TargetController&) = delete;
    TargetController& operator=(const TargetController&) = delete;

    // push strand 调用：输入 estimator 当期抖动观测 + 到达时钟（ns，限速时间基）。
    // underrun_events 是 JitterBuffer 的单调递增计数器（RT 线程写，这里只读
    // 快照，relaxed 足够）；传 0 或不传 = 关闭反馈（组件单独使用 / 单测）。
    // stall_peak_ms 是 estimator 的 stall 峰值（近期最坏到达间隙的衰减最大
    // 值）：margin = max(k×J, min(stall_peak/包周期 + 余量, CAP))，被 stall 门
    // 剔除出 J 的拥塞尾部由这项补回，cap 把孤立大 stall 挡在延迟债务之外。
    // 0 或不传 = 无峰值观测（退回纯 k×J）。
    // 返回本周期的 target（可能与上次相同；变化时调用方写 JB）。
    std::uint32_t update(double jitter_ms, std::int64_t arrival_ns,
        std::uint64_t underrun_events = 0, double stall_peak_ms = 0.0) noexcept;

    [[nodiscard]] std::uint32_t current() const noexcept { return current_; }
    [[nodiscard]] std::uint32_t min_target() const noexcept
    {
        return min_target_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t max_target() const noexcept { return max_target_; }
    // 当前欠载反馈抬升量（槽，诊断用；0 = 反馈未激活）。
    [[nodiscard]] double underrun_penalty() const noexcept
    {
        return penalty_.load(std::memory_order_relaxed);
    }
    // ---- target reason 诊断（上一次 update 的结算结果；push strand 独占读写）----
    // margin 的胜出方：kJ 还是 stall_peak。
    [[nodiscard]] TargetMarginSource margin_source() const noexcept
    {
        return margin_source_.load(std::memory_order_relaxed);
    }
    // desired 是否被下限抬起（地板/惩罚 binding = margin 失算，靠安全网托住）。
    [[nodiscard]] bool floor_bound() const noexcept
    {
        return floor_bound_.load(std::memory_order_relaxed);
    }
    // desired 是否被结构上限夹住（2/3 capacity 正在兜底）。
    [[nodiscard]] bool cap_bound() const noexcept
    {
        return cap_bound_.load(std::memory_order_relaxed);
    }
    // ---- 决策层诊断（上一次 update 的完整结算，push strand 独占读写）----
    // 未限速期望值（槽）：与 current() 不等即说明本拍被限速/dwell/死区按住。
    [[nodiscard]] std::uint32_t last_desired() const noexcept
    {
        return last_desired_.load(std::memory_order_relaxed);
    }
    // margin 两项各自的值（槽，取 max 之前）：k×J 与 stall 峰值项。
    [[nodiscard]] double last_jitter_margin() const noexcept { return last_jitter_margin_slots_; }
    [[nodiscard]] double last_stall_margin() const noexcept { return last_stall_margin_slots_; }
    // 本拍生效下限（= max(min_target, 地板+1) + 欠载惩罚，夹 max_target）。
    [[nodiscard]] std::uint32_t effective_min() const noexcept { return last_effective_min_; }
    // 本拍收敛路径 / 跌侧限速额度 / 涨后锁跌剩余。
    [[nodiscard]] TargetPath path() const noexcept { return path_; }
    [[nodiscard]] double fall_room_slots() const noexcept { return last_fall_room_slots_; }
    [[nodiscard]] double dwell_remaining_ms() const noexcept
    {
        return last_dwell_remaining_ms_.load(std::memory_order_relaxed);
    }
    // 死区配置（槽）；决策层诊断要能一眼看出 deadband 是否在吞变化。
    [[nodiscard]] std::uint32_t deadband_slots() const noexcept { return deadband_slots_; }
    // stall 峰值项上限（槽）。
    [[nodiscard]] double stall_peak_cap_slots() const noexcept { return stall_peak_cap_slots_; }

    void reset() noexcept;

private:
    [[nodiscard]] double compute_margin_slots(double jitter_ms) const noexcept;

    double packet_ms_;
    // min_target_slots 的构造参数（update_geometric_floor 重算下限时用）。
    // 构造后只读：写只在构造期（先于任何并发访问），无竞争。
    std::uint32_t min_target_param_ = 1;
    // 生效下限（= max(min_target_param_, 几何地板+1) 夹容量）。构造期与
    // update_geometric_floor（控制线程）写原子，push strand 的 update() 与
    // 诊断读 relaxed——一写多读。
    std::atomic<std::uint32_t> min_target_ { 1 };
    std::uint32_t max_target_;
    double jitter_gain_;
    double fall_rate_slots_per_sec_;
    std::uint32_t deadband_slots_;
    // stall 峰值项上限（槽）。声明位置与构造初始化列表一致（-Wreorder）。
    double stall_peak_cap_slots_;
    TargetMarginStrategy margin_strategy_;

    // 欠载反馈状态（push strand 写）
    // 原子化原因：underrun_penalty() 由**诊断线程**读取（client_runtime 的
    // take_diagnostics_snapshot），此前是普通 double 的跨线程无同步读写（UB）。
    // 单写者 + 多读者，relaxed load/store 即可。
    std::atomic<double> penalty_ { 0.0 };
    std::uint64_t last_underrun_events_ = 0;
    double penalty_per_event_ = 0.0;
    double penalty_max_ = 0.0;
    double penalty_decay_slots_per_sec_ = 0.0;

    double rise_dwell_ms_ = 0.0;
    std::int64_t last_rise_ns_ = 0; // 上次上涨时刻（ns）；dwell 窗口内锁跌

    std::uint32_t current_;
    std::uint32_t initial_;
    bool have_time_ = false;
    std::int64_t last_time_ns_ = 0;
    double fall_carry_ = 0.0; // 恢复限速的小数累积（包间隔远小于 1s 时仍精确限速）

    // target reason 诊断（push strand 独占，每次 update 结算）
    std::atomic<TargetMarginSource> margin_source_ { TargetMarginSource::Jitter };
    std::atomic<bool> floor_bound_ { false };
    std::atomic<bool> cap_bound_ { false };
    // 决策层诊断（同上；last_summary_ns_ 仅控制面日志开启时有意义）
    std::atomic<std::uint32_t> last_desired_ { 0 };
    double last_jitter_margin_slots_ = 0.0;
    double last_stall_margin_slots_ = 0.0;
    std::uint32_t last_effective_min_ = 0;
    double last_fall_room_slots_ = 0.0;
    std::atomic<double> last_dwell_remaining_ms_ { 0.0 };
    TargetPath path_ = TargetPath::Steady;
    std::int64_t last_summary_ns_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
