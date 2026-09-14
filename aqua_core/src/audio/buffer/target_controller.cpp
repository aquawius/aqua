#include "aqua/audio/buffer/target_controller.h"

#include "aqua/audio/buffer/buffer_config.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <cmath>
#include <utility>

// 控制面（决策层）日志开关：默认关闭。开启后本编译单元的决策日志会同步调用
// spdlog（内部有锁），组件因此不再满足"无 IO"约束——仅开发期使用。
// 边界：运行在 push strand / 控制线程上的决策与判定；RT 音频回调日志归
// AQUA_JB_RUNTIME_THREAD_DEBUG_LOG。点位全表见 doc/modules/observability.md。
#ifndef AQUA_JB_CONTROL_THREAD_DEBUG_LOG
#define AQUA_JB_CONTROL_THREAD_DEBUG_LOG 0
#endif

namespace aqua::audio {

namespace {

    constexpr double kNsPerSec = 1'000'000'000.0;
    constexpr double kNsPerMs = 1'000'000.0;

} // namespace

std::uint32_t TargetController::floor_target(const TargetControllerParams& params) noexcept
{
    const std::uint32_t capacity = std::max<std::uint32_t>(1, params.capacity_slots);
    // 硬下限 = max(min_target_slots, 几何地板 + 1)：几何地板无条件托底，
    // min_target_slots 只能往上抬。几何地板未提供（组件单独使用 / 单测）时该
    // 项为 0，下限退回 min_target_slots（并保证 ≥1）。
    const std::uint32_t geometric_floor
        = params.geometric_floor_slots != 0 ? params.geometric_floor_slots + 1u : 0u;
    const std::uint32_t floor = std::max<std::uint32_t>(
        geometric_floor, std::max<std::uint32_t>(1, params.min_target_slots));
    // 夹到容量之内：地板超过容量是病态配置（callback 一次要吃掉整个 buffer
    // 还多），此时可用的最小值就是容量本身。调用方（ClientRuntime 起步水位）
    // 拿到的必须是一个能直接用、且不会超过容量的值。
    return std::min(floor, capacity);
}

TargetController::TargetController(const TargetControllerParams& params) noexcept
    // min_target_ 必须在初始化列表里就夹到 max_target_ 之内：成员初始化顺序是
    // min 先于 max，不能引用 max_target_，只能重算同一个表达式。否则
    // std::clamp(initial, min, max) 在 min > max 时是 UB（几何地板超过容量的
    // 病态配置下会直接 fast-fail）。
    : packet_ms_(params.packet_ms > 0.0 ? params.packet_ms
                                        : config::JB_ADAPTIVE_DEFAULT_PACKET_MS)
    , min_target_param_(std::max<std::uint32_t>(1, params.min_target_slots))
    , min_target_(std::min(floor_target(params),
          std::max<std::uint32_t>(1, params.capacity_slots)))
    , max_target_(std::max<std::uint32_t>(1, params.capacity_slots))
    , jitter_gain_(params.jitter_gain >= 0.0 ? params.jitter_gain
                                             : config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN)
    , fall_rate_slots_per_sec_(
          params.fall_rate_slots_per_sec > 0.0 ? params.fall_rate_slots_per_sec
                                               : config::JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC)
    , deadband_slots_(params.deadband_slots)
    // 0 是合法极值（关闭 stall 峰值项），只有负值才退回默认——与 jitter_gain
    // 同口径（0/负值语义必须能区分，否则实验矩阵里的 0 点做不出来）。
    , stall_peak_cap_slots_(params.stall_peak_cap_slots >= 0.0
              ? params.stall_peak_cap_slots
              : config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS)
    , margin_strategy_(params.margin_strategy)
    // 0 是合法极值（关闭整条欠载反馈闭环），只有负值/非有限才退回默认——
    // 与 jitter_gain / stall_peak_cap 同口径（CLI help 与
    // configuration_reference.md §5.1 均承诺"负值 = 默认"）。
    , penalty_per_event_(
          params.underrun_penalty_per_event >= 0.0 ? params.underrun_penalty_per_event
                                                   : config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS)
    , penalty_max_(static_cast<double>(params.underrun_penalty_max_slots))
    , penalty_decay_slots_per_sec_(
          params.underrun_penalty_decay_slots_per_sec > 0.0
              ? params.underrun_penalty_decay_slots_per_sec
              : 0.0)
    , rise_dwell_ms_(params.rise_dwell_ms >= 0.0 ? params.rise_dwell_ms
                                                 : config::JB_ADAPTIVE_RISE_DWELL_MS)
    , current_(std::clamp(params.initial_target_slots,
          min_target_.load(std::memory_order_relaxed), max_target_))
    , initial_(current_)
{
    // min ≤ max 已由上面的初始化列表保证（夹取见 min_target_ 注释），此处无需
    // 再兜底——存量代码里的那段后验钳制反而掩盖了初始化列表的 UB。
}

void TargetController::update_geometric_floor(std::uint32_t geometric_floor_slots) noexcept
{
    // 与 floor_target() 同一口径（floor+1 托底、min_target_param_ 只能抬高、
    // 夹容量），只是作用在已构造的实例上。写原子即可：push strand 的
    // update() 下一次调用就会用新下限，current_ 的收敛方向见头文件注释。
    const std::uint32_t geometric_floor
        = geometric_floor_slots != 0 ? geometric_floor_slots + 1u : 0u;
    const auto new_min = std::min(std::max(geometric_floor, min_target_param_),
        max_target_);
    min_target_.store(new_min, std::memory_order_relaxed);
}

void TargetController::reset() noexcept
{
    current_ = initial_;
    have_time_ = false;
    last_time_ns_ = 0;
    penalty_.store(0.0, std::memory_order_relaxed);
    last_underrun_events_ = 0;
    last_rise_ns_ = 0;
    margin_source_.store(TargetMarginSource::Jitter, std::memory_order_relaxed);
    floor_bound_.store(false, std::memory_order_relaxed);
    cap_bound_.store(false, std::memory_order_relaxed);
    last_desired_.store(current_, std::memory_order_relaxed);
    last_jitter_margin_slots_ = 0.0;
    last_stall_margin_slots_ = 0.0;
    last_effective_min_ = min_target_.load(std::memory_order_relaxed);
    last_fall_room_slots_ = 0.0;
    last_dwell_remaining_ms_.store(0.0, std::memory_order_relaxed);
    path_ = TargetPath::Steady;
    last_summary_ns_ = 0;
}

double TargetController::compute_margin_slots(double jitter_ms) const noexcept
{
    switch (margin_strategy_) {
    case TargetMarginStrategy::ScaledJitter:
        return jitter_gain_ * jitter_ms / packet_ms_;
    }
    return 0.0; // 不可达：switch 覆盖全部枚举（-Wreturn-type 兜底）
}

std::uint32_t TargetController::update(
    double jitter_ms, std::int64_t arrival_ns,
    std::uint64_t underrun_events, double stall_peak_ms) noexcept
{
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
    // 本拍起点：决策日志要能说出"从哪到哪"（宏关时不需要）。
    const std::uint32_t previous_current = current_;
#endif

    // ---- 欠载反馈（细则 §3）：先结算惩罚，再算期望 ----
    // 计数器倒退只可能来自 JB reset（新会话），按"无新欠载"处理，不产生负增量。
    if (underrun_events > last_underrun_events_) {
        const auto delta = static_cast<double>(underrun_events - last_underrun_events_);
        penalty_.store(std::min(penalty_max_,
                           penalty_.load(std::memory_order_relaxed) + delta * penalty_per_event_),
            std::memory_order_relaxed);
    } else if (penalty_.load(std::memory_order_relaxed) > 0.0
        && penalty_decay_slots_per_sec_ > 0.0 && have_time_
        && arrival_ns > last_time_ns_) {
        const double decay = static_cast<double>(arrival_ns - last_time_ns_) / kNsPerSec
            * penalty_decay_slots_per_sec_;
        penalty_.store(
            std::max(0.0, penalty_.load(std::memory_order_relaxed) - decay),
            std::memory_order_relaxed);
    }
    last_underrun_events_ = underrun_events;

    // 期望 target（double 精度比较，落到整数槽时向上取整：宁多不少）。
    // 反馈项抬的是**下限**而不是加到 margin 上：这样 k×J 已经很高时不会重复
    // 叠加，而 k×J 失算（随机抖动尾部 / 丢包）时下限才真正起作用。
    const auto penalty_slots
        = static_cast<std::uint32_t>(penalty_.load(std::memory_order_relaxed));
    // 一次 update 内用同一份下限快照（update_geometric_floor 可能并发改写，
    // 分开读会拿到两个值拼出不一致的 effective_min）。
    const auto min_target = min_target_.load(std::memory_order_relaxed);
    // 64 位比较：原写法的 `max_target_ - min_target` 是无符号减法，一旦
    // min_target > max_target（病态配置/未来改动）就下溢成巨值，把 effective_min
    // 顶到荒谬槽位；加法也一并提到 64 位避免溢出。
    const auto effective_min
        = std::cmp_greater_equal(static_cast<std::uint64_t>(min_target) + penalty_slots,
              static_cast<std::uint64_t>(max_target_))
        ? max_target_
        : min_target + penalty_slots;
    const double jitter_margin_slots = compute_margin_slots(jitter_ms > 0.0 ? jitter_ms : 0.0);
    // stall 峰值项：近期最坏到达间隙换算成槽 + 余量（挺过间隙后水位不归零），
    // 再经 cap 限幅。与 k×J 取 max 而不是相加：两者都是"需要多少水"的估计，
    // stall 的亚阈值残余本来就在 J 里，相加会重复计。cap 的语义：stall 是
    // "已经发生的恢复风险信号"，不是 steady-state 延迟要求——孤立大 stall
    // （60ms+）不该买入十几槽延迟债务（实测 59.6ms → 7→17 → DROP 还债风暴）；
    // cap 内的线性段覆盖常态/中度拥塞（下载实测：J≈5ms → 7~8 槽，25~50ms
    // stall 对应 8~14 槽，cap 8 槽恰好接管 26ms 以下的部分），超出的交给
    // 欠载惩罚 + reanchor。极端情形最终还有 max_target_（2/3 容量）兜底。
    const double stall_margin_slots = stall_peak_ms > 0.0
        ? std::min(stall_peak_ms / packet_ms_ + config::JB_ADAPTIVE_STALL_PEAK_EXTRA_PACKETS,
              stall_peak_cap_slots_)
        : 0.0;
    const double margin_slots = std::max(jitter_margin_slots, stall_margin_slots);
    // target reason 结算：margin 胜出方 + desired 被下限/上限夹持的状态。
    margin_source_.store(
        stall_margin_slots > jitter_margin_slots ? TargetMarginSource::StallPeak
                                                 : TargetMarginSource::Jitter,
        std::memory_order_relaxed);
    floor_bound_.store(margin_slots < static_cast<double>(effective_min),
        std::memory_order_relaxed);
    cap_bound_.store(margin_slots > static_cast<double>(max_target_),
        std::memory_order_relaxed);
    const auto desired = static_cast<std::uint32_t>(
        std::ceil(std::clamp(margin_slots,
            static_cast<double>(effective_min), static_cast<double>(max_target_))));

    // ---- 决策层诊断结算：本拍全量状态（日志/诊断读，不参与控制律）----
    last_desired_.store(desired, std::memory_order_relaxed);
    last_jitter_margin_slots_ = jitter_margin_slots;
    last_stall_margin_slots_ = stall_margin_slots;
    last_effective_min_ = effective_min;
    last_fall_room_slots_ = 0.0;
    last_dwell_remaining_ms_.store(0.0, std::memory_order_relaxed);
    path_ = TargetPath::Steady;

    if (std::cmp_greater(desired, static_cast<std::uint64_t>(current_) + deadband_slots_)) {
        // 恶化：超死区即立即跟进（涨快），限速余量清零，并记下上涨时刻——
        // dwell 窗口以此锁跌（峰值保持）。
        current_ = desired;
        fall_carry_ = 0.0;
        last_rise_ns_ = arrival_ns;
        path_ = TargetPath::Rise;
    } else if (desired < current_) {
        // 恢复：不限死区，一律 grind 到期望值——跌侧由 fall_rate 限速，
        // 死区只防“涨”抽动；跌侧死区会把干净网钉在 min+deadband 下不来。
        if (!have_time_ || arrival_ns <= last_time_ns_) {
            current_ = desired; // 首拍无时间基 / 时钟异常：全额跟进
            fall_carry_ = 0.0;
            path_ = TargetPath::NoTimeBase;
        } else if (rise_dwell_ms_ > 0.0
            && (arrival_ns - last_rise_ns_)
                < static_cast<std::int64_t>(rise_dwell_ms_ * kNsPerMs)) {
            // 涨后 dwell 窗口内锁跌：J 摆动期 target 钉在较高值，只在窗口外
            // 才允许缓慢回落。锁跌期间不攒限速余量，否则窗口一过会跳变。
            fall_carry_ = 0.0;
            path_ = TargetPath::DwellLock;
            last_dwell_remaining_ms_.store(
                rise_dwell_ms_ - static_cast<double>(arrival_ns - last_rise_ns_) / kNsPerMs,
                std::memory_order_relaxed);
        } else {
            // 本拍限速额度（槽）：fall_rate × Δt。它是"为什么只降了这一格"的
            // 直接答案，故单独留档而不是只体现在 current_ 的差值里。
            last_fall_room_slots_ = (static_cast<double>(arrival_ns - last_time_ns_) / kNsPerSec)
                * fall_rate_slots_per_sec_;
            fall_carry_ += last_fall_room_slots_;
            const double room = static_cast<double>(current_ - desired);
            const auto step = static_cast<std::uint32_t>(std::min(fall_carry_, room));
            current_ -= step;
            fall_carry_ = (current_ == desired) ? 0.0 : fall_carry_ - static_cast<double>(step);
            path_ = TargetPath::Fall;
        }
    } else {
        // 死区内：不清零会攒出一次跳变，直接清。
        fall_carry_ = 0.0;
        if (desired != current_) {
            // desired 高于 current 但差值 ≤ deadband：被死区吞掉（不是稳态）。
            path_ = TargetPath::Deadband;
        }
    }
    have_time_ = true;
    last_time_ns_ = arrival_ns;

    // 控制面日志（#1，见本文件顶部说明）：target 变化 = 事件驱动全量；
    // 稳态按 config::JB_CONTROL_LOG_SUMMARY_INTERVAL_MS 节流一行摘要。
    // 两档字段完全相同，便于直接 diff 变化前后。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
    const bool target_changed = current_ != previous_current;
    const bool summary_due = last_summary_ns_ == 0
        || (arrival_ns - last_summary_ns_)
            >= static_cast<std::int64_t>(config::JB_CONTROL_LOG_SUMMARY_INTERVAL_MS * kNsPerMs);
    if (target_changed || summary_due) {
        log_debug_fmt(
            "TargetController {}: current {} -> {} desired={} margin={:.2f}[kJ {:.2f} | stall {:.2f}] src={} floor_bind={} cap_bind={} effective_min={} penalty={:.2f} path={} fall_room={:.2f} dwell_left={:.0f}ms deadband={} jit_ms={:.2f} stall_peak_ms={:.1f} stall_cap={:.1f}",
            target_changed ? "change" : "steady",
            previous_current, current_,
            last_desired_.load(std::memory_order_relaxed),
            std::max(last_jitter_margin_slots_, last_stall_margin_slots_),
            last_jitter_margin_slots_, last_stall_margin_slots_,
            target_margin_source_name(margin_source_.load(std::memory_order_relaxed)),
            floor_bound_.load(std::memory_order_relaxed) ? 1 : 0,
            cap_bound_.load(std::memory_order_relaxed) ? 1 : 0,
            last_effective_min_, penalty_.load(std::memory_order_relaxed),
            target_path_name(path_), last_fall_room_slots_,
            last_dwell_remaining_ms_.load(std::memory_order_relaxed), deadband_slots_,
            jitter_ms > 0.0 ? jitter_ms : 0.0, stall_peak_ms, stall_peak_cap_slots_);
        last_summary_ns_ = arrival_ns;
    }
#endif
    return current_;
}

} // namespace aqua::audio
