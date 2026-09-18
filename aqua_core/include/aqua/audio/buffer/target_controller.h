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
// margin = max(尾部分位数 P99/包周期 + 1，min(stall 峰值/包周期 + 余量, 上限))。
// P99 只在尾部运动时搬家（均值噪声碰不到它）；冷启动窗口未满时抖动项为 0，
// target 直接落到地板（floor start，见 update 注释）。
// stall 峰值（NetEq 式 peak detection）补拥塞尾部，cap 把"孤立大 stall"
// 挡在 steady-state 延迟债务之外。effective_min = 几何地板/min_target +
// 欠载惩罚（feedback 只抬下限不叠 margin，抖动项高时不重复放大）。
// target 的物理意义 = "JB 该持有多少已到达的数据"；网络底噪/单程延迟
// （estimator 的 base_delay）不是 buffer budget，已移出公式（诊断保留）。
// 风暴端稳（stall 频率驱动）：断流频发期冻结一切下跌（涨仍即时），
// target 端稳不动；风暴过后恢复正常跌速。进快（窗口频率超阈值）出慢
// （连续完整窗口零事件才退出）。
//
// 历史注记：v1 的 k×J 抖动项（ScaledJitter）在大版本重构中已删除，只剩一种
// margin 哲学——P99 预测 + stall 峰值补尾 + penalty 兜底。k×J 的覆盖域（冷启动
// 0.5s、60ms 级快响应）缺席代价有上界（地板蹲半秒，几个被掩盖的单槽），
// 实测无感；真需要关自适应时用 fixed-target 模式（JitterBuffer 直连固定水位带）。
// estimator 的 J（RFC 3550 测量值）保留，只当诊断（`jit=` 列），不进控制律。
//
// 控制迟滞（细则 §4）：涨快跌慢。恶化立即跟进，恢复按 fall_rate
// 限速，杜绝 target 来回抽动（死区恒 0，见 ADR-4，参数已删除）。
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

// 跨语言契约：TargetPath / TargetMarginSource 没有对应的 C 枚举常量，JNI 直接把内部
// 取值当 int32 写进诊断数组、Kotlin 用 when(...) 解码展示。因此**这两个枚举的数值顺序
// 是契约的一部分**：重排/插值必须在同一提交里同步 Kotlin 的 when 分支。
// 下面把它们钉死，任何重排都会在编译期失败（见文件末尾的 static_assert 组）。
// （大版本重构注：v1 的 Jitter=0 已删除，TailQuantile 从 2 → 0，Kotlin 侧同步改。）

// 计罚欠载计数选择：决定什么欠载才抬 target 下限。
//
// 目标是"允许小的音频质量欠缺下的低延迟"：concealment 开启时，孤立的短缺口
// （≤ 掩盖上限，默认 3 包 ≈ 11ms）被 repeat-last + 淡出盖住，人耳基本不可闻——
// 为这种缺口买延迟是亏的。只有掩盖封顶溢出（concealed_saturated_slots）的部分
// 才是真正变成静音/可闻的缺损，值得抬地板。concealment 关闭时每个缺口都是静音，
// 退回 underrun_events（与旧行为逐字一致）。
//
// 本函数是 ClientRuntime 与离线回放 harness 共用的唯一口径（与
// apply_adaptive_bands 同理）：两处若各写一份，仿真里 penalty 的触发条件会与
// 实际悄悄分叉。controller 本体只认"计罚计数"，不感知 concealment 开关。
[[nodiscard]] inline std::uint64_t select_penalty_events(bool concealment_enabled,
    std::uint64_t underrun_events, std::uint64_t concealed_saturated_slots) noexcept
{
    return concealment_enabled ? concealed_saturated_slots : underrun_events;
}

// 最近一次 update 中 margin 的胜出方（诊断用）：target 为什么变必须可解释，
// 否则只能从 tail/stall_peak/penalty 倒推。
enum class TargetMarginSource : std::uint8_t { TailQuantile = 0,
    StallPeak = 1 };

[[nodiscard]] inline const char* target_margin_source_name(TargetMarginSource s) noexcept
{
    switch (s) {
    case TargetMarginSource::StallPeak:
        return "stall_peak";
    case TargetMarginSource::TailQuantile:
        break;
    }
    return "tail_p99";
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
    StormHold = 6, // 风暴期（stall 频发）冻结下跌，端稳等待风暴过去
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
    case TargetPath::StormHold:
        return "storm_hold";
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
    // 是结构性不可能（例：F=175@48k + 512 帧 callback → floor=ceil(512/175)=3，故 target 至少 floor+1=4）。target 必须
    // 至少 floor+1：一个 callback 的口粮 + 一包余量垫住到达相位。
    // 0 = 调用方未提供（组件单独使用 / 单测），此时退化为 min_target_slots。
    std::uint32_t geometric_floor_slots = 0;
    // 恢复限速：每秒最多降这么多。只锁**下跌**——上涨永远即时。
    double fall_rate_slots_per_sec = config::JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC;
    // 上涨后的峰值保持窗口（ms）：窗口内不允许下跌。为什么需要（J 在 ceil
    // 边界摆动导致 target 7↔8 抽动）见 config::JB_ADAPTIVE_RISE_DWELL_MS。
    double rise_dwell_ms = config::JB_ADAPTIVE_RISE_DWELL_MS;
    // ---- 欠载反馈（细则 §3：underrun history 是 controller 的输入）----
    // 抖动项（P99）覆盖不了随机尾部与丢包；反馈项补这个洞：发生可闻欠载就把
    // target 的**下限**顶上去，一段时间不再欠载再慢慢放下。
    // 它是安全网不是主力，干净链路上恒为 0。取值理由见
    // config::JB_ADAPTIVE_UNDERRUN_PENALTY_*（buffer_config.h）。
    double underrun_penalty_per_event
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS; // 每次欠载事件抬升的下限（槽）。
    // 0 = 关闭整条反馈闭环（合法极值，对照实验用）；负值/非有限 = 退回默认。
    std::uint32_t underrun_penalty_max_slots
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS; // 反馈项累计上限（防病态放大）
    double underrun_penalty_decay_slots_per_sec
        = config::JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC; // 无新欠载时的回落速率
    // （历史注记：死区参数已删除——非零死区与跌侧 grind 叠加产生永久偏移，
    // 见 ADR-4；死区恒为 0，不再是可调项。）
    // stall 峰值项的上限（槽）= `--jb-stall-peak-cap`。stall 是"已经发生的恢复
    // 风险信号"，不是新的 steady-state 延迟要求；本值决定一次孤立大 stall 最多
    // 把 target 推多高（超过的部分交给欠载惩罚 + concealment + reanchor 各管一段）。
    // 取值理由见 config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS。**0 = 关闭 stall 峰值
    // 项**（margin 只剩尾部分位数），可用于分离"预测项"的贡献；负值 = 默认值。
    double stall_peak_cap_slots = config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
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

    // push strand 调用：输入 estimator 当期观测 + 到达时钟（ns，限速时间基）。
    // tail_p99_ms 是唯一的抖动项（尾部分位数/包周期 + 1 包相位余量，≥0 才有效）：
    // 负值 = 无尾部观测（冷启动约 0.5s / 旧调用方），此时抖动项为 0，desired
    // 直接落到地板（floor start）——冷启动缺口由 concealment 盖住、可闻缺口由
    // penalty 事后补，不需要第二套预测公式。
    // penalty_events 是"计罚欠载计数"（单调递增，RT 线程写，这里只读快照，
    // relaxed 足够）；调用方经 select_penalty_events() 映射后传入（conceal 开时
    // 为 saturated slots，否则为 JB 的 underrun_events）。传 0 或不传 = 关闭
    // 反馈（组件单独使用 / 单测）。注意：这里故意只抬"可闻"欠载——被掩盖住的
    // 孤立短缺口不计罚（见 select_penalty_events），这是低延迟目标的核心取舍。
    // stall_peak_ms 是 estimator 的 stall 峰值（近期最坏到达间隙的衰减最大
    // 值）：margin = max(尾部分位数项, min(stall_peak/包周期 + 余量, CAP))，
    // 被 stall 门剔除的拥塞尾部由这项补回，cap 把孤立大 stall 挡在延迟债务之外。
    // 0 或不传 = 无峰值观测（退回纯尾部项）。
    // stall_events 是 estimator 的 stall 事件累计计数：风暴判定（频率驱动）
    // 的输入；传 0 = 无风暴观测（风暴端稳永不触发）。单测传什么都行（默认 0）。
    // 返回本周期的 target（可能与上次相同；变化时调用方写 JB）。
    std::uint32_t update(std::int64_t arrival_ns, std::uint64_t penalty_events = 0,
        double stall_peak_ms = 0.0, double tail_p99_ms = -1.0,
        std::uint64_t stall_events = 0) noexcept;

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
    // margin 的胜出方：tail_p99 还是 stall_peak。
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
    // ---- 决策层诊断（上一次 update 的完整结算）----
    // 写侧只有 push strand 的 update()；**读侧还有诊断线程**（client_runtime 的
    // take_diagnostics_snapshot 可在任意线程调用）→ 凡被快照读走的成员必须是原子
    // （relaxed 足够：单写多读、不承担同步语义）。已原子：margin_source_/
    // floor_bound_/cap_bound_/last_desired_/dwell_remaining_ms_/path_/
    // last_fall_room_slots_。max_target_ 构造后不变（const），无需原子。
    // 未限速期望值（槽）：与 current() 不等即说明本拍被限速/dwell/死区按住。
    [[nodiscard]] std::uint32_t last_desired() const noexcept
    {
        return last_desired_.load(std::memory_order_relaxed);
    }
    // margin 两项各自的值（槽，取 max 之前）：尾部分位数项与 stall 峰值项。
    [[nodiscard]] double last_tail_margin() const noexcept { return last_tail_margin_slots_; }
    [[nodiscard]] double last_stall_margin() const noexcept { return last_stall_margin_slots_; }
    // 本拍生效下限（= max(min_target, 地板+1) + 欠载惩罚，夹 max_target）。
    [[nodiscard]] std::uint32_t effective_min() const noexcept { return last_effective_min_; }
    // 本拍收敛路径 / 跌侧限速额度 / 涨后锁跌剩余。
    [[nodiscard]] TargetPath path() const noexcept
    {
        return path_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double fall_room_slots() const noexcept
    {
        return last_fall_room_slots_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double dwell_remaining_ms() const noexcept
    {
        return last_dwell_remaining_ms_.load(std::memory_order_relaxed);
    }
    // stall 峰值项上限（槽）。
    [[nodiscard]] double stall_peak_cap_slots() const noexcept { return stall_peak_cap_slots_; }

    void reset() noexcept;

private:
    double packet_ms_;
    // min_target_slots 的构造参数（update_geometric_floor 重算下限时用）。
    // 构造后只读：写只在构造期（先于任何并发访问），无竞争。
    std::uint32_t min_target_param_ = 1;
    // 生效下限（= max(min_target_param_, 几何地板+1) 夹容量）。构造期与
    // update_geometric_floor（控制线程）写原子，push strand 的 update() 与
    // 诊断读 relaxed——一写多读。
    std::atomic<std::uint32_t> min_target_ { 1 };
    const std::uint32_t max_target_; // 构造后不变（快照线程读也安全）
    double fall_rate_slots_per_sec_;
    // stall 峰值项上限（槽）。声明位置与构造初始化列表一致（-Wreorder）。
    double stall_peak_cap_slots_;

    // 欠载反馈状态（push strand 写）
    // 原子化原因：underrun_penalty() 由**诊断线程**读取（client_runtime 的
    // take_diagnostics_snapshot），此前是普通 double 的跨线程无同步读写（UB）。
    // 单写者 + 多读者，relaxed load/store 即可。
    std::atomic<double> penalty_ { 0.0 };
    // 上次见到的计罚计数（select_penalty_events 映射后的口径，不是原始 underrun）。
    std::uint64_t last_penalty_events_ = 0;
    double penalty_per_event_ = 0.0;
    double penalty_max_ = 0.0;
    double penalty_decay_slots_per_sec_ = 0.0;

    // 风暴判定状态（push strand 独占）：stall 事件计数的 tumbling 窗口。
    // storm_active_ 为真时冻结一切下跌（涨仍即时）。诊断经 path_=StormHold 暴露。
    std::uint64_t last_stall_count_ = 0;
    std::int64_t storm_window_start_ns_ = 0;
    std::uint32_t storm_window_count_ = 0;
    bool have_storm_time_ = false;
    bool storm_active_ = false;

    double rise_dwell_ms_ = 0.0;
    std::int64_t last_rise_ns_ = 0; // 上次上涨时刻（ns）；dwell 窗口内锁跌

    std::uint32_t current_;
    std::uint32_t initial_;
    bool have_time_ = false;
    std::int64_t last_time_ns_ = 0;
    double fall_carry_ = 0.0; // 恢复限速的小数累积（包间隔远小于 1s 时仍精确限速）

    // target reason 诊断（push strand 独占，每次 update 结算）
    std::atomic<TargetMarginSource> margin_source_ { TargetMarginSource::TailQuantile };
    std::atomic<bool> floor_bound_ { false };
    std::atomic<bool> cap_bound_ { false };
    // 决策层诊断（同上；last_summary_ns_ 仅控制面日志开启时有意义）
    std::atomic<std::uint32_t> last_desired_ { 0 };
    double last_tail_margin_slots_ = 0.0;
    double last_stall_margin_slots_ = 0.0;
    std::uint32_t last_effective_min_ = 0;
    std::atomic<double> last_fall_room_slots_ { 0.0 }; // 诊断线程读（jc.fall_room_slots），故原子
    std::atomic<double> last_dwell_remaining_ms_ { 0.0 };
    // 诊断线程读（jc.path）→ 原子；其余"诊断结算"成员（tail/stall margin、
    // effective_min）只有同线程读者（update 自身日志 + 单测），不需要原子。
    std::atomic<TargetPath> path_ { TargetPath::Steady };
    std::int64_t last_summary_ns_ = 0;
};

// ---- 枚举数值契约（见文件顶的跨语言契约说明）----
// （大版本重构注：v1 的 TargetMarginSource::Jitter=0 已删除，TailQuantile 从 2 → 0，
// Kotlin 侧 when 分支同步改。TargetPath 数值未动。）
static_assert(static_cast<int>(TargetPath::Steady) == 0, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::Rise) == 1, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::Fall) == 2, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::DwellLock) == 3, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::Deadband) == 4, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::NoTimeBase) == 5, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetPath::StormHold) == 6, "TargetPath 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetMarginSource::TailQuantile) == 0, "TargetMarginSource 数值是 JNI/Kotlin 契约");
static_assert(static_cast<int>(TargetMarginSource::StallPeak) == 1, "TargetMarginSource 数值是 JNI/Kotlin 契约");

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
