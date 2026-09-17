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

#include "aqua/audio/buffer/buffer_config.h"

#include <atomic>
#include <cstdint>

namespace aqua::audio {

// 滑动观测窗：[max_ext - (W-1), max_ext] 内见过的 seq 标记位。
// duplicate = 窗内已见；reordered = 窗内未见但落后 max（乱但可能有用）；
// late = 落后窗口之外（Phase 0 只计数，是否利用由 Phase 0/1 数据决定）。
// 窗宽 W 见 config::JB_ESTIMATOR_REORDER_WINDOW_PACKETS（buffer_config.h）。

struct JitterEstimates {
    double transit_ms = 0.0; // 相对 transit（随机 offset 已消去，见 transit() 注释）
    // ---- transit 电平（纯诊断，不进控制）----
    // transit 的慢跟随（EWMA，系数见 JB_TRANSIT_LEVEL_GAIN）：抖动只让它轻晃，
    // 路径延迟电平变化（AP 切换/路由跃迁/队列均衡点移动）会让它整段搬家。
    // base 是"历史最好"，level 是"现在在哪"——level - base 持续拉大 = 路径变差。
    double transit_level_ms = 0.0; // transit 慢跟随电平
    std::uint64_t transit_step_events = 0; // 单包 |Δtransit| ≥ 阈值（包单位）的次数
    double last_transit_step_ms = 0.0; // 最近一次台阶的带符号幅度（+ = 路径变差）
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
    // 近期最坏到达间隙的衰减最大值（NetEq 式 peak detection）：每次 stall
    // 刷新为 max(峰值, 本次间隙)，无 stall 时线性衰减（速率见
    // config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC）。TargetController
    // 用它把 target 抬到能挺过近期最坏间隙的水位——被 stall 门剔除出 J
    // 的尾部由这项补回。
    double stall_peak_ms = 0.0;
    // ---- 尾部直方图（抖动项的输入，唯一的预测项）----
    // 单包绝对偏差 |到达间隔 - 发送间隔| 的滑动窗口 P99。
    // 刻意不用 transit - base：累积最小 base 在持续漂移/阶跃下永不更新，
    // 相对值会永远钉高位；差分天然漂移不变（漂移由 episodes + penalty 负责，
    // margin 只需覆盖网络抖动）。J 是均值，一包噪声就动；P99 只在尾部运动时
    // 搬家，所以拿它当抖动项（诊断 `tailm`/`p99`/`tsamp` 列）。
    // 窗内样本 < JB_TAIL_MIN_SAMPLES 时发布 -1（reset 后同）：controller 按
    // tail<0 取抖动项 0 落地板。**0.0 是合法测量值**（干净链路 P99 就是 0），不能当哨兵。
    double tail_p99_ms = -1.0; // 窗内单包偏差的 P99（ms）；<0 = 无尾部观测
    std::uint64_t tail_samples = 0; // 窗内有效样本数（满窗 = JB_TAIL_WINDOW_PACKETS）
};

class JitterEstimator {
public:
    // timestamp_rate_hz = RTP timestamp 时钟（即 sample_rate）；
    // frames_per_packet = 每包媒体帧数（决定包周期，用于 stall 判定）。
    // stall_threshold_packet_periods：到达间隔超过这么多**个包周期**即判为 stall
    // （时间断流）并从 J 中剔除。默认值与取值理由见
    // config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS（buffer_config.h）。
    // **≤ 0 = 关闭 stall 检测**（每个间隔都进 J，回到裸 RFC 3550）——这是验证
    // "stall 剔除到底有没有用"的唯一 A/B 手段。
    // stall_peak_decay_ms_per_sec：stall 峰值的衰减速度（ms/s）。**0 = 峰值永久
    // 保持**（极值实验：把"近期最坏间隙"变成"历史最坏间隙"）；负值 = 默认值。
    // 取值理由见 config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC。
    // 非法参数退化为 rate=1（不崩溃；ClientRuntime 传入前已校验）。
    explicit JitterEstimator(std::uint32_t timestamp_rate_hz, std::uint32_t frames_per_packet,
        double stall_threshold_packet_periods
        = config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS,
        double stall_peak_decay_ms_per_sec
        = config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC) noexcept;

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
    const double packet_ms_ = 0.0; // 一个包的媒体时长（ms），stall/台阶阈值的时间基
    const double stall_threshold_ms_ = 0.0; // = packet_ms_ × stall_threshold_packet_periods
    const double transit_step_threshold_ms_ = 0.0; // = packet_ms_ × JB_TRANSIT_STEP_PACKETS
    const double stall_peak_decay_ms_per_sec_ = 0.0; // stall 峰值衰减速率（0 = 不衰减）

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
    double transit_level_ms_ = 0.0; // strand 封闭：transit 慢跟随电平（EWMA）
    double prev_transit_ms_ = 0.0; // strand 封闭：上一样本 transit（台阶做相邻差分）
    double base_delay_ms_ = 0.0;
    // base 是否已确立。不能拿 base == 0.0 当"未初始化"哨兵：干净链路上
    // transit 恒为 0，哨兵每包都成立，首个 spike 会把 base 抬成 spike 值，
    // 相对时延（transit - base）归零——尾部直方图与 base 诊断同时变脏。
    bool base_set_ = false;
    // stall 峰值跟踪（strand 封闭）：decayed max of recent stall gaps。
    // stall_peak_last_ns_ = 上次衰减结算时刻；0 = 尚未有结算基线。
    double stall_peak_ms_ = 0.0;
    std::int64_t stall_peak_last_ns_ = 0;
    // 尾部直方图（strand 封闭，默认策略的抖动项输入）：单包偏差滑动窗口。
    // ring 存桶序号（止于 JB_TAIL_WINDOW_PACKETS），hist 做增量计数
    // （新样本 +1，滑出样本 -1），P99 每包现算 64 桶线性扫。
    // 约束与 J 本体一致：strand 内 O(1)/零分配/无锁，对外只经原子发布。
    double tail_ring_[config::JB_TAIL_WINDOW_PACKETS] = { }; // 环内样本的桶序号（小整数，double 存取精确）
    std::uint32_t tail_head_ = 0; // 下一个写入位置
    std::uint32_t tail_count_ = 0; // 窗内有效样本数（≤ WINDOW）
    std::uint32_t tail_hist_[config::JB_TAIL_HISTOGRAM_BUCKETS + 1] = { };
    // 控制面日志的里程碑计数（#3：J 从 0 到收敛的 1/16/64/256 样本）。
    // 仅 AQUA_JB_CONTROL_THREAD_DEBUG_LOG 开启时递增，缺省构建下恒为 0。
    std::uint64_t jitter_sample_count_ = 0;

    // 对外 gauge/counter（原子，x64 lock-free；诊断线程读）。
    std::atomic<double> transit_ms_ { 0.0 };
    std::atomic<double> transit_level_ms_out_ { 0.0 };
    std::atomic<std::uint64_t> transit_step_events_ { 0 };
    std::atomic<double> last_transit_step_ms_ { 0.0 };
    std::atomic<double> jitter_ms_out_ { 0.0 };
    std::atomic<double> base_delay_ms_out_ { 0.0 };
    // 初值与 reset() / 冷启动门一致：-1 = 无尾部观测（含"构造后、首个包到达前"那一拍）。
    std::atomic<double> tail_p99_ms_out_ { -1.0 };
    std::atomic<std::uint64_t> tail_samples_out_ { 0 };
    std::atomic<double> arrival_interval_ms_ { 0.0 };
    std::atomic<std::uint64_t> packets_ { 0 };
    std::atomic<std::uint64_t> duplicates_ { 0 };
    std::atomic<std::uint64_t> reordered_ { 0 };
    std::atomic<std::uint64_t> late_ { 0 };
    std::atomic<std::uint64_t> gap_events_ { 0 };
    std::atomic<std::uint64_t> missing_packets_ { 0 };
    std::atomic<std::uint64_t> stall_events_ { 0 };
    std::atomic<double> last_stall_gap_ms_ { 0.0 };
    std::atomic<double> stall_peak_ms_out_ { 0.0 };
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_ESTIMATOR_H
