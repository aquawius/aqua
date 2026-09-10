#ifndef AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
#define AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H

// Phase 1 自适应延迟改造：只负责计算 target，不执行播放。
//
// 职责边界（见 JB 自适应改造细则 §1/§3）：
//   JitterEstimator = 只负责观察网络
//   TargetController = 只负责计算 target_slots
//   JitterBuffer = 继续负责实际播放与 Fill/Drop/reanchor
//
// 第一版算法（细则 §3.1）：target = clamp(base + k×J, min, max)。
// margin 策略必须可替换（percentile/histogram/peak/hybrid 留给未来）：
// MarginStrategy 枚举 + compute_margin() 分支就是扩展点，不要把公式焊死。
//
// 控制迟滞（细则 §4）：涨快跌慢 + 死区。恶化立即跟进，恢复按 fall_rate
// 限速，deadband 内变化忽略，杜绝 target 来回抽动。
//
// 约束：无 IO、无锁、无分配、O(1) 每次 update；只在 push strand 调用。

#include <cstdint>

namespace aqua::audio {

// margin 策略扩展点（Phase 1 只有 ScaledJitter；加策略 = 加枚举 + 分支）。
enum class TargetMarginStrategy : std::uint8_t { ScaledJitter = 0 };

struct TargetControllerParams {
    std::uint32_t capacity_slots = 30; // 上限来源：target 永不超过 capacity
    double packet_ms = 10.0; // 包时长 = F×1000/sample_rate（ms）
    std::uint32_t min_target_slots = 3; // 保底：防单包抖动饿死（实测 2 slots 在
    // F=3.75ms 链路上正好落在欠载悬崖下：2 slots = 16.6% 欠载 + 25% 丢帧，
    // 3 slots = 0%。不要把下限压到 2。）
    std::uint32_t initial_target_slots = 4; // 起步：快启与安全的折中
    double jitter_gain = 2.0; // k：margin = k×J（包单位）
    double fall_rate_slots_per_sec = 1.0; // 恢复限速：每秒最多降这么多
    // 死区：期望与当前差值在该范围内不动。**默认 0** —— 死区与"跌侧不限死区
    // grind 到底"叠加会产生永久偏移：跌到 desired 后，desired 回升 ≤deadband
    // 被吞掉，target 永远停在 desired−1（实测 target 卡 2 而 desired=3）。
    // 阻尼由跌侧限速提供（涨快跌慢 = 峰值保持 + 缓慢衰减，本身不振荡）。
    std::uint32_t deadband_slots = 0;
    TargetMarginStrategy margin_strategy = TargetMarginStrategy::ScaledJitter;
};

class TargetController {
public:
    explicit TargetController(const TargetControllerParams& params) noexcept;

    TargetController(const TargetController&) = delete;
    TargetController& operator=(const TargetController&) = delete;

    // push strand 调用：输入 estimator 当期观测 + 到达时钟（ns，限速时间基）。
    // 返回本周期的 target（可能与上次相同；变化时调用方写 JB）。
    std::uint32_t update(double base_delay_ms, double jitter_ms, std::int64_t arrival_ns) noexcept;

    [[nodiscard]] std::uint32_t current() const noexcept { return current_; }
    [[nodiscard]] std::uint32_t min_target() const noexcept { return min_target_; }
    [[nodiscard]] std::uint32_t max_target() const noexcept { return max_target_; }

    void reset() noexcept;

private:
    [[nodiscard]] double compute_margin_slots(double jitter_ms) const noexcept;

    double packet_ms_;
    std::uint32_t min_target_;
    std::uint32_t max_target_;
    double jitter_gain_;
    double fall_rate_slots_per_sec_;
    std::uint32_t deadband_slots_;
    TargetMarginStrategy margin_strategy_;

    std::uint32_t current_;
    std::uint32_t initial_;
    bool have_time_ = false;
    std::int64_t last_time_ns_ = 0;
    double fall_carry_ = 0.0; // 恢复限速的小数累积（包间隔远小于 1s 时仍精确限速）
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
