#include "aqua/audio/buffer/target_controller.h"

#include <algorithm>
#include <cmath>

namespace aqua::audio {

namespace {

    constexpr double kNsPerSec = 1'000'000'000.0;

} // namespace

TargetController::TargetController(const TargetControllerParams& params) noexcept
    : packet_ms_(params.packet_ms > 0.0 ? params.packet_ms : 10.0)
    , min_target_(std::max<std::uint32_t>(1, params.min_target_slots))
    , max_target_(std::max<std::uint32_t>(1, params.capacity_slots))
    , jitter_gain_(params.jitter_gain >= 0.0 ? params.jitter_gain : 2.0)
    , fall_rate_slots_per_sec_(
          params.fall_rate_slots_per_sec > 0.0 ? params.fall_rate_slots_per_sec : 1.0)
    , deadband_slots_(params.deadband_slots)
    , margin_strategy_(params.margin_strategy)
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
    double base_delay_ms, double jitter_ms, std::int64_t arrival_ns) noexcept
{
    // 期望 target（double 精度比较，落到整数槽时向上取整：宁多不少）。
    const double base_slots = base_delay_ms > 0.0 ? base_delay_ms / packet_ms_ : 0.0;
    const double margin_slots = compute_margin_slots(jitter_ms > 0.0 ? jitter_ms : 0.0);
    const auto desired = static_cast<std::uint32_t>(
        std::ceil(std::clamp(base_slots + margin_slots,
            static_cast<double>(min_target_), static_cast<double>(max_target_))));

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
