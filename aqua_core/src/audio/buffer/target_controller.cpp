#include "aqua/audio/buffer/target_controller.h"

#include "aqua/audio/buffer/buffer_config.h"

#include <algorithm>
#include <cmath>

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
    , min_target_(std::min(floor_target(params),
          std::max<std::uint32_t>(1, params.capacity_slots)))
    , max_target_(std::max<std::uint32_t>(1, params.capacity_slots))
    , jitter_gain_(params.jitter_gain >= 0.0 ? params.jitter_gain
          : config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN)
    , fall_rate_slots_per_sec_(
          params.fall_rate_slots_per_sec > 0.0 ? params.fall_rate_slots_per_sec
                                              : config::JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC)
    , deadband_slots_(params.deadband_slots)
    , margin_strategy_(params.margin_strategy)
    , penalty_per_event_(
          params.underrun_penalty_per_event > 0.0 ? params.underrun_penalty_per_event : 0.0)
    , penalty_max_(static_cast<double>(params.underrun_penalty_max_slots))
    , penalty_decay_slots_per_sec_(
          params.underrun_penalty_decay_slots_per_sec > 0.0
              ? params.underrun_penalty_decay_slots_per_sec
              : 0.0)
    , rise_dwell_ms_(params.rise_dwell_ms >= 0.0 ? params.rise_dwell_ms
          : config::JB_ADAPTIVE_RISE_DWELL_MS)
    , current_(std::clamp(params.initial_target_slots, min_target_, max_target_))
    , initial_(current_)
{
    // min ≤ max 已由上面的初始化列表保证（夹取见 min_target_ 注释），此处无需
    // 再兜底——存量代码里的那段后验钳制反而掩盖了初始化列表的 UB。
}

void TargetController::reset() noexcept
{
    current_ = initial_;
    have_time_ = false;
    last_time_ns_ = 0;
    penalty_ = 0.0;
    last_underrun_events_ = 0;
    last_rise_ns_ = 0;
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
    double base_delay_ms, double jitter_ms, std::int64_t arrival_ns,
    std::uint64_t underrun_events) noexcept
{
    // ---- 欠载反馈（细则 §3）：先结算惩罚，再算期望 ----
    // 计数器倒退只可能来自 JB reset（新会话），按"无新欠载"处理，不产生负增量。
    if (underrun_events > last_underrun_events_) {
        const auto delta = static_cast<double>(underrun_events - last_underrun_events_);
        penalty_ = std::min(penalty_max_, penalty_ + delta * penalty_per_event_);
    } else if (penalty_ > 0.0 && penalty_decay_slots_per_sec_ > 0.0 && have_time_
        && arrival_ns > last_time_ns_) {
        const double decay = static_cast<double>(arrival_ns - last_time_ns_) / kNsPerSec
            * penalty_decay_slots_per_sec_;
        penalty_ = std::max(0.0, penalty_ - decay);
    }
    last_underrun_events_ = underrun_events;

    // 期望 target（double 精度比较，落到整数槽时向上取整：宁多不少）。
    // 反馈项抬的是**下限**而不是加到 margin 上：这样 k×J 已经很高时不会重复
    // 叠加，而 k×J 失算（随机抖动尾部 / 丢包）时下限才真正起作用。
    const auto penalty_slots = static_cast<std::uint32_t>(penalty_);
    const auto effective_min = (penalty_slots >= max_target_ - min_target_)
        ? max_target_
        : min_target_ + penalty_slots;
    const double base_slots = base_delay_ms > 0.0 ? base_delay_ms / packet_ms_ : 0.0;
    const double margin_slots = compute_margin_slots(jitter_ms > 0.0 ? jitter_ms : 0.0);
    const auto desired = static_cast<std::uint32_t>(
        std::ceil(std::clamp(base_slots + margin_slots,
            static_cast<double>(effective_min), static_cast<double>(max_target_))));

    if (desired > current_ + deadband_slots_) {
        // 恶化：超死区即立即跟进（涨快），限速余量清零，并记下上涨时刻——
        // dwell 窗口以此锁跌（峰值保持）。
        current_ = desired;
        fall_carry_ = 0.0;
        last_rise_ns_ = arrival_ns;
    } else if (desired < current_) {
        // 恢复：不限死区，一律 grind 到期望值——跌侧由 fall_rate 限速，
        // 死区只防“涨”抽动；跌侧死区会把干净网钉在 min+deadband 下不来。
        if (!have_time_ || arrival_ns <= last_time_ns_) {
            current_ = desired; // 首拍无时间基 / 时钟异常：全额跟进
            fall_carry_ = 0.0;
        } else if (rise_dwell_ms_ > 0.0
            && (arrival_ns - last_rise_ns_)
                < static_cast<std::int64_t>(rise_dwell_ms_ * kNsPerMs)) {
            // 涨后 dwell 窗口内锁跌：J 摆动期 target 钉在较高值，只在窗口外
            // 才允许缓慢回落。锁跌期间不攒限速余量，否则窗口一过会跳变。
            fall_carry_ = 0.0;
        } else {
            fall_carry_ += (static_cast<double>(arrival_ns - last_time_ns_) / kNsPerSec)
                * fall_rate_slots_per_sec_;
            const double room = static_cast<double>(current_ - desired);
            const auto step = static_cast<std::uint32_t>(std::min(fall_carry_, room));
            current_ -= step;
            fall_carry_ = (current_ == desired) ? 0.0 : fall_carry_ - static_cast<double>(step);
        }
    } else {
        // 死区内：不清零会攒出一次跳变，直接清。
        fall_carry_ = 0.0;
    }
    have_time_ = true;
    last_time_ns_ = arrival_ns;
    return current_;
}

} // namespace aqua::audio
