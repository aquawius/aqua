#ifndef AQUA_AUDIO_BUFFER_JITTER_ESTIMATOR_H
#define AQUA_AUDIO_BUFFER_JITTER_ESTIMATOR_H

// Phase 0 自适应延迟改造：纯网络观测量（只观察，不驱动 JB）。
//
// 职责边界（见 JB 自适应改造细则 §1）：
//   JitterEstimator = 只负责观察网络（arrival statistics）
//   TargetController（Phase 1） = 只负责计算 target
//   JitterBuffer = 继续负责实际播放与 Fill/Drop/reanchor
// Estimator 不直接操作 JB，不触发任何 recovery action。
//
// 约束：无 IO、无锁（gauge/counter 用 lock-free 原子对外，x64 上无等待）、
// 无动态分配、O(1) 每包。实例只在 UDP/network push strand 上 update，
// pull/RT playback 侧只读 estimates() 快照。
//
// 输入是 wire 观测三元组（seq16 + RTP timestamp + 到达时钟），不依赖 JB 内部
// 状态：timestamp 跳变/非单调、丢包、乱序在这里只记统计，不变成 JB 动作。

#include <atomic>
#include <cstdint>

namespace aqua::audio {

// 64 包滑动观测窗：[max_ext-63, max_ext] 内见过的标记位。
// duplicate = 窗内已见；reordered = 窗内未见但落后 max（乱但可能有用）；
// late = 落后窗口之外（Phase 0 只计数，是否利用由 Phase 0/1 数据决定）。
inline constexpr std::uint64_t kEstimatorReorderWindow = 64;

struct JitterEstimates {
    double transit_ms = 0.0; // 相对 transit（随机 offset 已消去，见 transit() 注释）
    double jitter_ms = 0.0; // RFC 3550 A.8 interarrival jitter J（观测量 ≠ target）
    double base_delay_ms = 0.0; // 路径底噪：transit 累积最小值（非 clock drift 补偿）
    double arrival_interval_ms = 0.0; // 相邻包到达间隔（诊断用）
    std::uint64_t packets = 0; // 观测包总数（含 duplicate，不含 SSRC 切换前的旧流）
    std::uint64_t duplicates = 0; // 窗内已见 seq 重复到达
    std::uint64_t reordered = 0; // 落后 max 但在窗内（乱序到达）
    std::uint64_t late = 0; // 落后窗口之外（太晚，Phase 0 只计数不利用）
    std::uint64_t gap_events = 0; // ext 跳跃事件（seq 缺口，与 JB 缺口统计口径独立）
    std::uint64_t missing_packets = 0; // 缺口累计缺失包数
    // stall（时间断流，与 seq 缺口独立）：到达间隔超过 stall_threshold 个包
    // 周期。这类事件不进 RFC3550 J（见 jitter_estimator.cpp），只在这里计数。
    std::uint64_t stall_events = 0;
    double last_stall_gap_ms = 0.0; // 最近一次 stall 的到达间隔（诊断）
};

class JitterEstimator {
public:
    // timestamp_rate_hz = RTP timestamp 时钟（即 sample_rate）；
    // frames_per_packet = 每包媒体帧数（决定包周期，用于 stall 判定）。
    // stall_threshold_packets：到达间隔超过这么多**个包周期**即判为 stall
    // （时间断流）并从 J 中剔除。默认 5：burst 发包的正常串间间隔约 2.7 个
    // 包周期，留近一倍余量；35ms 级及以上的真 stall（双机实测 35~170ms）
    // 稳稳落在线外。非法参数退化为 rate=1（不崩溃；ClientRuntime 传入前已校验）。
    explicit JitterEstimator(std::uint32_t timestamp_rate_hz, std::uint32_t frames_per_packet,
        double stall_threshold_packets = 5.0) noexcept;

    JitterEstimator(const JitterEstimator&) = delete;
    JitterEstimator& operator=(const JitterEstimator&) = delete;

    // 观测一个解码后的 Audio 包（push strand 调用；JB 是否接受它与此无关，
    // 观测的是网络本身）。arrival_ns = steady 时钟到达时刻。
    // SSRC 变化 = 新流，内部全重置后按首包处理。
    void observe(std::uint16_t seq, std::uint32_t timestamp, std::uint32_t ssrc,
        std::int64_t arrival_ns) noexcept;

    // 跨线程读快照（supervision/诊断线程）；原子 relaxed 读，不保证多字段一致。
    [[nodiscard]] JitterEstimates estimates() const noexcept;

    // 会话重建时显式重置（SSRC 切换由 observe 内部自动处理）。
    void reset() noexcept;

private:
    const double timestamp_rate_hz_;
    const double packet_ms_ = 0.0; // 一个包的媒体时长（ms），stall 阈值的时间基
    const double stall_threshold_ms_ = 0.0; // = packet_ms_ × stall_threshold_packets

    // strand 封闭状态（仅 update 侧读写）。
    bool have_packets_ = false;
    std::uint32_t current_ssrc_ = 0;
    std::uint64_t max_ext_ = 0; // 见过的最大 extended sequence
    std::uint64_t window_bits_ = 0; // [max-63, max] 见位（bit i 对应 max-i）
    std::uint32_t last_timestamp_ = 0;
    std::int64_t last_arrival_ns_ = 0;
    // transit 锚点（首包 / 时间轴重置时建立；timestamp 随机 offset 在此消去）。
    std::uint32_t anchor_timestamp_ = 0;
    std::int64_t anchor_arrival_ns_ = 0;
    double jitter_ms_ = 0.0;
    double base_delay_ms_ = 0.0;

    // 对外 gauge/counter（原子，x64 lock-free；诊断线程读）。
    std::atomic<double> transit_ms_ { 0.0 };
    std::atomic<double> jitter_ms_out_ { 0.0 };
    std::atomic<double> base_delay_ms_out_ { 0.0 };
    std::atomic<double> arrival_interval_ms_ { 0.0 };
    std::atomic<std::uint64_t> packets_ { 0 };
    std::atomic<std::uint64_t> duplicates_ { 0 };
    std::atomic<std::uint64_t> reordered_ { 0 };
    std::atomic<std::uint64_t> late_ { 0 };
    std::atomic<std::uint64_t> gap_events_ { 0 };
    std::atomic<std::uint64_t> missing_packets_ { 0 };
    std::atomic<std::uint64_t> stall_events_ { 0 };
    std::atomic<double> last_stall_gap_ms_ { 0.0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_ESTIMATOR_H
