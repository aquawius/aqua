#include "aqua/audio/buffer/target_controller.h"

#include "aqua/audio/buffer/buffer_config.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// 控制面（决策层）日志开关：默认关闭。开启后本编译单元的决策日志会同步调用
// spdlog（内部有锁），组件因此不再满足"无 IO"约束——仅开发期使用。
// 边界：运行在 push strand / 控制线程上的决策与判定；RT 音频回调日志归
// AQUA_CLIENT_RT_DEBUG_LOG。点位全表见 doc/modules/observability.md。
#ifndef AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
#define AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG 0
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
    , fall_rate_slots_per_sec_(
          params.fall_rate_slots_per_sec > 0.0 ? params.fall_rate_slots_per_sec
                                                : config::JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC)
    // 0 是合法极值（关闭 stall 峰值项），只有负值才退回默认——0/负值语义必须
    // 能区分，否则实验矩阵里的 0 点做不出来。
    , stall_peak_cap_slots_(params.stall_peak_cap_slots >= 0.0
              ? params.stall_peak_cap_slots
              : config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS)
    // 0 是合法极值（关闭整条欠载反馈闭环），只有负值/非有限才退回默认——
    // 与 stall_peak_cap 同口径（CLI help 与 configuration_reference.md §5.1
    // 均承诺"负值 = 默认"）。
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
    // 跌侧限速余量（fall_carry_）是跨拍累积量：不复位会让复位后的第一次下跌
    // 把上个会话攒下的余量一次性用掉，多降一格（跌侧限速形同失效）。
    fall_carry_ = 0.0;
    have_time_ = false;
    last_time_ns_ = 0;
    penalty_.store(0.0, std::memory_order_relaxed);
    last_penalty_events_ = 0;
    last_rise_ns_ = 0;
    margin_source_.store(TargetMarginSource::TailQuantile, std::memory_order_relaxed);
    floor_bound_.store(false, std::memory_order_relaxed);
    cap_bound_.store(false, std::memory_order_relaxed);
    last_desired_.store(current_, std::memory_order_relaxed);
    last_stall_count_ = 0;
    storm_window_start_ns_ = 0;
    storm_window_count_ = 0;
    have_storm_time_ = false;
    storm_active_.store(false, std::memory_order_relaxed);
    last_tail_margin_slots_ = 0.0;
    last_stall_margin_slots_ = 0.0;
    last_effective_min_ = min_target_.load(std::memory_order_relaxed);
    last_fall_room_slots_ = 0.0;
    last_dwell_remaining_ms_.store(0.0, std::memory_order_relaxed);
    path_ = TargetPath::Steady;
    last_summary_ns_ = 0;
}

std::uint32_t TargetController::update(std::int64_t arrival_ns,
    std::uint64_t penalty_events, double stall_peak_ms, double tail_p99_ms,
    std::uint64_t stall_events) noexcept
{
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
    // 本拍起点：决策日志要能说出"从哪到哪"（宏关时不需要）。
    const std::uint32_t previous_current = current_;
#endif

    // ---- 欠载反馈（细则 §3）：先结算惩罚，再算期望 ----
    // 计数器倒退只可能来自 JB reset（新会话），按"无新欠载"处理，不产生负增量。
    if (penalty_events > last_penalty_events_) {
        const auto delta = static_cast<double>(penalty_events - last_penalty_events_);
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
    last_penalty_events_ = penalty_events;

    // 期望 target（double 精度比较，落到整数槽时向上取整：宁多不少）。
    // 反馈项抬的是**下限**而不是加到 margin 上：这样尾部项已经很高时不会重复
    // 叠加，而尾部失算（随机尾部 / 丢包）时下限才真正起作用。
    // double → uint 截断是有意的容忍死区（不是 bug）：penalty < 1.0 时下限不动。
    // 单次可闻欠载 +1.0、按 0.5/s 衰减，因此零星可闻缺口（衰减到 1.0 以下）不会
    // 长期钉住地板；诊断 pen= 显示衰减中的 double，而这里用截断后的整数——两者
    // 不一致是预期的，effective_min 才是本拍真正的下限。
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
    // 抖动项（唯一的预测项）：尾部分位数 P99/包周期 + 相位余量
    // （JB_TAIL_PHASE_MARGIN_PACKETS，与 stall 项的 +1 同哲学）。P99 只在尾部
    // 运动时搬家，均值噪声碰不到它。
    // tail<0（冷启动约 0.5s / 旧调用方）时抖动项为 0，desired 直接落到地板
    // （floor start）：冷启动缺口由 concealment 盖住、可闻缺口由 penalty 事后补。
    const double tail_margin_slots = tail_p99_ms >= 0.0 && packet_ms_ > 0.0
        ? tail_p99_ms / packet_ms_ + config::JB_TAIL_PHASE_MARGIN_PACKETS
        : 0.0;
    // stall 峰值项：近期最坏到达间隙换算成槽 + 余量（挺过间隙后水位不归零），
    // 再经 cap 限幅。与尾部项取 max 而不是相加：两者都是"需要多少水"的估计，
    // 相加会重复计。cap 的语义：stall 是"已经发生的恢复风险信号"，不是
    // steady-state 延迟要求——孤立大 stall（60ms+）不该买入十几槽延迟债务
    // （实测 59.6ms → 7→17 → DROP 还债风暴）；cap 内的线性段覆盖常态/中度拥塞，
    // 超出的交给欠载惩罚 + reanchor。极端情形最终还有 max_target_（2/3 容量）兜底。
    const double stall_margin_slots = stall_peak_ms > 0.0
        ? std::min(stall_peak_ms / packet_ms_ + config::JB_ADAPTIVE_STALL_PEAK_EXTRA_PACKETS,
              stall_peak_cap_slots_)
        : 0.0;
    const double margin_slots = std::max(tail_margin_slots, stall_margin_slots);
    // target reason 结算：margin 胜出方 + desired 被下限/上限夹持的状态。
    margin_source_.store(
        stall_margin_slots > tail_margin_slots ? TargetMarginSource::StallPeak
                                               : TargetMarginSource::TailQuantile,
        std::memory_order_relaxed);
    floor_bound_.store(margin_slots < static_cast<double>(effective_min),
        std::memory_order_relaxed);
    cap_bound_.store(margin_slots > static_cast<double>(max_target_),
        std::memory_order_relaxed);
    const auto desired = static_cast<std::uint32_t>(
        std::ceil(std::clamp(margin_slots,
            static_cast<double>(effective_min), static_cast<double>(max_target_))));

    // ---- 风暴判定（stall 频率驱动的两档模式）----
    // tumbling 窗口计数：窗口内频率 ≥ 阈值进入风暴；连续一个完整窗口零事件
    // 才退出（进快出慢）。风暴期冻结一切下跌（涨仍即时），target 端稳。
    // stall 事件是"断流正在发生"的先行信号（与 underrun"已欠载"的事后信号
    // 不同），用它做模式切换不需要等到听感受损。
    bool storm_hold = false;
    if (stall_events > last_stall_count_) {
        storm_window_count_ += static_cast<std::uint32_t>(
            std::min<std::uint64_t>(stall_events - last_stall_count_,
                std::numeric_limits<std::uint32_t>::max()));
    }
    last_stall_count_ = stall_events;
    {
        constexpr std::int64_t kNsPerMsStorm = 1'000'000;
        const auto window_ns = static_cast<std::int64_t>(
                                   config::JB_STORM_WINDOW_MS)
            * kNsPerMsStorm;
        if (!have_storm_time_ || arrival_ns < storm_window_start_ns_) {
            // 首拍 / 时钟倒退：开新窗口，不判定（风暴只影响下跌，首拍走全额路径）。
            // 注意不清空 count：当前拍的增量已在上面计入，清掉会让进入判定
            // 永远晚一个窗口。
            storm_window_start_ns_ = arrival_ns;
            have_storm_time_ = true;
        } else if (arrival_ns - storm_window_start_ns_ >= window_ns) {
            const double window_sec = static_cast<double>(
                                          arrival_ns - storm_window_start_ns_)
                / 1'000'000'000.0;
            const double rate = window_sec > 0.0
                ? static_cast<double>(storm_window_count_) / window_sec
                : 0.0;
            if (storm_active_.load(std::memory_order_relaxed)) {
                // 在暴中：非零即续命，完整窗口零事件才退出（出慢）。
                storm_active_.store(storm_window_count_ > 0, std::memory_order_relaxed);
            } else {
                storm_active_.store(rate >= config::JB_STORM_ENTER_EVENTS_PER_SEC,
                    std::memory_order_relaxed);
            }
            storm_window_start_ns_ = arrival_ns;
            storm_window_count_ = 0;
        }
        storm_hold = storm_active_.load(std::memory_order_relaxed);
    }

    // ---- 决策层诊断结算：本拍全量状态（日志/诊断读，不参与控制律）----
    last_desired_.store(desired, std::memory_order_relaxed);
    last_tail_margin_slots_ = tail_margin_slots;
    last_stall_margin_slots_ = stall_margin_slots;
    last_effective_min_ = effective_min;
    last_fall_room_slots_ = 0.0;
    last_dwell_remaining_ms_.store(0.0, std::memory_order_relaxed);
    path_ = TargetPath::Steady;

    // 死区恒为 0（ADR-4，参数已删除）：desired > current 即涨，不存在"被吞的变化"。
    if (desired > current_) {
        // 恶化即立即跟进（涨快），限速余量清零，并记下上涨时刻——
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
        } else if (storm_hold) {
            // 风暴期（stall 频发）冻结下跌：不断追 spike 只会让 target 上蹿下跳，
            // 端稳等风暴过去再按正常跌速回落。锁跌期间同样不攒限速余量。
            // 涨分支不受影响（恶化永远即时跟进）。
            fall_carry_ = 0.0;
            path_ = TargetPath::StormHold;
        } else if (rise_dwell_ms_ > 0.0
            && (arrival_ns - last_rise_ns_)
                < static_cast<std::int64_t>(rise_dwell_ms_ * kNsPerMs)) {
            // 涨后 dwell 窗口内锁跌：抖动项摆动期 target 钉在较高值，只在窗口外
            // 才允许缓慢回落。锁跌期间不攒限速余量，否则窗口一过会跳变。
            fall_carry_ = 0.0;
            path_ = TargetPath::DwellLock;
            last_dwell_remaining_ms_.store(
                rise_dwell_ms_ - static_cast<double>(arrival_ns - last_rise_ns_) / kNsPerMs,
                std::memory_order_relaxed);
        } else {
            // 本拍限速额度（槽）：fall_rate × Δt。它是"为什么只降了这一格"的
            // 直接答案，故单独留档而不是只体现在 current_ 的差值里。
            // 远距加速：current 高出 desired 足够多（风暴过后的 overhang）时按
            // 倍率快跌，否则慢跌速下 19→4 要 75 秒，期间水位偏高 FILL 不断。
            // 近平衡区（<4 槽）保持慢速——chatter 抑制不受影响。
            const double room_now = static_cast<double>(current_ - desired);
            const double far_multiple = room_now
                    >= static_cast<double>(config::JB_FALL_FAR_DISTANCE_SLOTS)
                ? config::JB_FALL_FAR_RATE_MULTIPLE
                : 1.0;
            last_fall_room_slots_ = (static_cast<double>(arrival_ns - last_time_ns_) / kNsPerSec)
                * fall_rate_slots_per_sec_ * far_multiple;
            fall_carry_ += last_fall_room_slots_;
            const double room = static_cast<double>(current_ - desired);
            const auto step = static_cast<std::uint32_t>(std::min(fall_carry_, room));
            current_ -= step;
            fall_carry_ = (current_ == desired) ? 0.0 : fall_carry_ - static_cast<double>(step);
            path_ = TargetPath::Fall;
        }
    } else {
        // desired == current（稳态）：限速余量不清零会攒出一次跳变，直接清。
        // （死区恒 0，能到这里一定是相等；TargetPath::Deadband 枚举保留给
        // 跨语言契约，永不可达。）
        fall_carry_ = 0.0;
    }
    have_time_ = true;
    last_time_ns_ = arrival_ns;

    // 控制面日志（#1，见本文件顶部说明）：target 变化 = 事件驱动全量；
    // 稳态按 config::JB_CONTROL_LOG_SUMMARY_INTERVAL_MS 节流一行摘要。
    // 两档字段完全相同，便于直接 diff 变化前后。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
    const bool target_changed = current_ != previous_current;
    const bool summary_due = last_summary_ns_ == 0
        || (arrival_ns - last_summary_ns_)
            >= static_cast<std::int64_t>(config::JB_CONTROL_LOG_SUMMARY_INTERVAL_MS * kNsPerMs);
    if (target_changed || summary_due) {
        log_debug_fmt(
            "TargetController {}: current {} -> {} desired={} margin={:.2f}[tail {:.2f} | stall {:.2f}] src={} floor_bind={} cap_bind={} effective_min={} penalty={:.2f} path={} fall_room={:.2f} dwell_left={:.0f}ms storm={} stall_peak_ms={:.1f} stall_cap={:.1f} tail_p99_ms={:.1f}",
            target_changed ? "change" : "steady",
            previous_current, current_,
            last_desired_.load(std::memory_order_relaxed),
            std::max(last_tail_margin_slots_, last_stall_margin_slots_),
            last_tail_margin_slots_,
            last_stall_margin_slots_,
            target_margin_source_name(margin_source_.load(std::memory_order_relaxed)),
            floor_bound_.load(std::memory_order_relaxed) ? 1 : 0,
            cap_bound_.load(std::memory_order_relaxed) ? 1 : 0,
            last_effective_min_, penalty_.load(std::memory_order_relaxed),
            target_path_name(path_.load(std::memory_order_relaxed)),
            last_fall_room_slots_.load(std::memory_order_relaxed), // 原子成员不能直接进 fmt
            last_dwell_remaining_ms_.load(std::memory_order_relaxed),
            storm_active_.load(std::memory_order_relaxed) ? 1 : 0,
            stall_peak_ms, stall_peak_cap_slots_,
            tail_p99_ms >= 0.0 ? tail_p99_ms : 0.0);
        last_summary_ns_ = arrival_ns;
    }
#endif
    return current_;
}

} // namespace aqua::audio
