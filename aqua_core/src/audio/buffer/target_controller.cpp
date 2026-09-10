#include "aqua/audio/buffer/target_controller.h"

#include <algorithm>
#include <cmath>

namespace aqua::audio {

namespace {

    constexpr double kNsPerSec = 1'000'000'000.0;

} // namespace

TargetController::TargetController(const TargetControllerParams& params) noexcept
    : packet_ms_(params.packet_ms > 0.0 ? params.packet_ms : 10.0)
    , min_target_(std::max<std::uint32_t>(
          // 几何地板优先：pull_grant+1 是"一个 callback 口粮 + 一包垫相位"，
          // 低于它无论抖动多小都必然周期性排空（与抖动无关的结构性下限）。
          params.pull_grant_slots != 0 ? params.pull_grant_slots + 1u : 0u,
          std::max<std::uint32_t>(1, params.min_target_slots)))
    , max_target_(std::max<std::uint32_t>(1, params.capacity_slots))
    , jitter_gain_(params.jitter_gain >= 0.0 ? params.jitter_gain : 5.0)
    , fall_rate_slots_per_sec_(
          params.fall_rate_slots_per_sec > 0.0 ? params.fall_rate_slots_per_sec : 1.0)
    , deadband_slots_(params.deadband_slots)
    , margin_strategy_(params.margin_strategy)
    , penalty_per_event_(
          params.underrun_penalty_per_event > 0.0 ? params.underrun_penalty_per_event : 0.0)
    , penalty_max_(static_cast<double>(params.underrun_penalty_max_slots))
    , penalty_decay_slots_per_sec_(
          params.underrun_penalty_decay_slots_per_sec > 0.0
              ? params.underrun_penalty_decay_slots_per_sec
              : 0.0)
    , current_(std::clamp(params.initial_target_slots, min_target_, max_target_))
    , initial_(current_)
{
    if (min_target_ > max_target_) {
        min_target_ = max_target_;
        current_ = std::clamp(current_, min_target_, max_target_);
    }
}

void TargetController::reset() noexcept
{
    current_ = initial_;
    have_time_ = false;
    last_time_ns_ = 0;
    penalty_ = 0.0;
    last_underrun_events_ = 0;
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
        // 恶化：超死区即立即跟进（涨快），限速余量清零。
        current_ = desired;
        fall_carry_ = 0.0;
    } else if (desired < current_) {
        // 恢复：不限死区，一律 grind 到期望值——跌侧由 fall_rate 限速，
        // 死区只防“涨”抽动；跌侧死区会把干净网钉在 min+deadband 下不来。
        if (!have_time_ || arrival_ns <= last_time_ns_) {
            current_ = desired; // 首拍无时间基 / 时钟异常：全额跟进
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
