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
    // 几何地板：一次 playback callback 要消耗 ceil(callback_frames / F) 个包。
    // target ≤ 该值意味着"每个 callback 都必然把 JB 抽空"——这不是抖动问题，
    // 是结构性不可能（F=180@48k + 512 帧 callback → floor=3）。target 必须
    // 至少 floor+1：一个 callback 的口粮 + 一包余量垫住到达相位。
    // 0 = 调用方未提供（组件单独使用 / 单测），此时退化为 min_target_slots。
    std::uint32_t geometric_floor_slots = 0;
    // k：margin = k×J（包单位）。
    //
    // 为什么不是教科书的 2~3：J（RFC 3550 A.8）是 |到达间隔偏差| 的**均值**，
    // 而 target 必须覆盖**峰值**。Aqua 的 server 以 capture 周期成串发包
    // （480 帧/10ms 抓一次，180 帧/包 → 每 10ms 一串 2~3 个包，串内间隔≈0），
    // 这种确定性 burst 下 J≈4.6ms 而实际峰峰值 8.75ms≈2 倍均值；再叠加
    // callback 周期（10.667ms）与发包周期（10ms）的拍频，拖到最坏相位时
    // k=2 给出的 3 slots 会周期性排空（双机实测 6.5% 欠载 + 12% 丢帧）。
    // 离线仿真（同几何、扫 16 个相位取最坏）中 k≥5 才把欠载压到 0。
    // 干净/匀速链路上 J→0，margin→0，target 落到几何地板，不会过度缓冲。
    double jitter_gain = 5.0;
    double fall_rate_slots_per_sec = 1.0; // 恢复限速：每秒最多降这么多
    // 上涨后的峰值保持窗口（ms）：窗口内不允许下跌。
    //
    // 为什么需要：J 随发包几何在 ceil 边界上下摆动（Wi-Fi 省电时发包变成
    // ~20ms 一大串，J 在 4.7↔5.4ms 间跳，margin 6.3↔7.2 → ceil 7↔8），
    // 跌侧 1 槽/秒的限速又不断制造下跌腿，于是 target 反复横跨 7↔8。
    // 单看翻转无所谓（lead 在 normal 带内就不触发 Fill/Drop），但 target 一旦
    // 落到偏低的 7，lead 就会撞上 normal_high 触发 Drop（实测 drop_duty 1.4%）。
    // 涨后 dwell 内锁跌 = 峰值保持：J 摆动期间 target 钉在较高值，只在窗口
    // 之外才允许缓慢回落。上涨永远即时（恶化必须立即跟进），dwell 只锁跌。
    double rise_dwell_ms = 3000.0;
    // ---- 欠载反馈（细则 §3 明确列为 controller 输入，Phase 1 未接）----
    // 预测项 k×J 用的是**均值**，覆盖不了随机抖动的尾部，更覆盖不了丢包；
    // 这两类情况在真实网络里都会漏进欠载。反馈项补这个洞：发生欠载就把
    // target 的下限顶上去，一段时间不再欠载再慢慢放下来。它是"安全网"，
    // 不是主力——主力仍是 k×J，所以干净链路上 penalty 恒为 0，不增延迟。
    double underrun_penalty_per_event = 1.0; // 每次欠载事件抬升的下限（槽）
    std::uint32_t underrun_penalty_max_slots = 6; // 反馈项累计上限（防病态放大）
    double underrun_penalty_decay_slots_per_sec = 0.5; // 无新欠载时的回落速率
    // 死区：期望与当前差值在该范围内不动。**默认 0** —— 死区与"跌侧不限死区
    // grind 到底"叠加会产生永久偏移：跌到 desired 后，desired 回升 ≤deadband
    // 被吞掉，target 永远停在 desired−1（实测 target 卡 2 而 desired=3）。
    // 阻尼由跌侧限速提供（涨快跌慢 = 峰值保持 + 缓慢衰减，本身不振荡）。
    std::uint32_t deadband_slots = 0;
    TargetMarginStrategy margin_strategy = TargetMarginStrategy::ScaledJitter;
};

class TargetController {
public:
    // 给定参数算出 target 的硬下限。构造前即可调用（ClientRuntime 需要它决定
    // 起步水位，而那时 controller 还没建），构造与运行期共用同一份口径——
    // 地板逻辑只此一处，不要在两个地方各算一遍。
    [[nodiscard]] static std::uint32_t floor_target(const TargetControllerParams& params) noexcept;

    explicit TargetController(const TargetControllerParams& params) noexcept;

    TargetController(const TargetController&) = delete;
    TargetController& operator=(const TargetController&) = delete;

    // push strand 调用：输入 estimator 当期观测 + 到达时钟（ns，限速时间基）。
    // underrun_events 是 JitterBuffer 的单调递增计数器（RT 线程写，这里只读
    // 快照，relaxed 足够）；传 0 或不传 = 关闭反馈（组件单独使用 / 单测）。
    // 返回本周期的 target（可能与上次相同；变化时调用方写 JB）。
    std::uint32_t update(double base_delay_ms, double jitter_ms, std::int64_t arrival_ns,
        std::uint64_t underrun_events = 0) noexcept;

    [[nodiscard]] std::uint32_t current() const noexcept { return current_; }
    [[nodiscard]] std::uint32_t min_target() const noexcept { return min_target_; }
    [[nodiscard]] std::uint32_t max_target() const noexcept { return max_target_; }
    // 当前欠载反馈抬升量（槽，诊断用；0 = 反馈未激活）。
    [[nodiscard]] double underrun_penalty() const noexcept { return penalty_; }

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

    // 欠载反馈状态（push strand 独占）
    double penalty_ = 0.0;
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
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_TARGET_CONTROLLER_H
