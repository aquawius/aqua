#ifndef AQUA_DIAGNOSTICS_MANAGER_H
#define AQUA_DIAGNOSTICS_MANAGER_H

#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/jitter_buffer/jitter_buffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace aqua::diag {

// DiagnosticsManager：M5 诊断数据采集与输出。
//
// 从 JitterBuffer 和 RingBuffer 采集指标，计算 interarrival jitter、
// RingBuffer occupancy slope（short/long 窗口）、端到端缓冲延迟、
// 时钟漂移（server 发送速率 vs 客户端播放速率的双回归），周期性输出日志。
//
// 线程模型：
//   - 事件回调（record_packet_arrival / record_hello_sent / record_hello_ack_received /
//     record_underrun / record_deadline_miss）在 io_context 线程或播放线程调用。
//   - 周期采样（record_rb_occupancy / collect_and_log）在 client 主线程调用。
//   跨线程共享的数据用 atomic（relaxed）或 mutex 保护，详见各字段注释。
class DiagnosticsManager {
public:
    // 采样回调：返回当前 RingBuffer available_read 字节数
    using RingBufferFillFn = std::function<std::size_t()>;
    // 播放进度回调：返回 client 已播放的累计样本数（跨 JB 播放累积）
    using PlayedSamplesFn = std::function<std::uint64_t()>;

    // rb_capacity_bytes: RingBuffer 总容量（字节），用于诊断日志显示水位/容量比
    DiagnosticsManager(std::uint32_t sample_rate,
        std::size_t frame_bytes,
        std::size_t payload_size,
        RingBufferFillFn rb_fill_fn,
        std::size_t rb_capacity_bytes,
        PlayedSamplesFn played_samples_fn = { });

    // ---- 事件回调（在 io_context / 播放线程调用）----

    // 记录一个音频包到达：更新 interarrival jitter，并采样 (到达时间, sample_position)
    // 供 server 发送速率回归使用。
    void record_packet_arrival(std::uint32_t sequence, std::uint32_t sample_position);

    // RTT 测量：发送 HELLO 时记录起点（需在发出 HELLO 前调用）
    void record_hello_sent();

    // RTT 测量：收到 HELLO_ACK 时记录终点并计算往返时延
    void record_hello_ack_received();

    // 记录一次播放欠载（WASAPI 回调返回不足时）
    void record_underrun();

    // 记录一次调度错过 deadline（JB timer 延迟超过 1 个 packet_duration 时）
    void record_deadline_miss();

    // 记录一次播放缓冲重臂（pre-roll latch re-arm：饥饿 3 连空仓或低水位看门狗）。
    // 每次重臂伴随一次短静音，是运行点自愈次数的直接指标。
    void record_rb_rearm();

    // 记录收到的音频字节数（payload only）
    void record_audio_bytes(std::size_t bytes);

    // 记录收到的 HELLO_ACK
    void record_hello_ack();

    // ---- 主线程周期采样 ----

    // 高频（~500ms，client 主循环 RB_SAMPLE_INTERVAL）：采样 RingBuffer 占用存入
    // slope 窗口，并采样播放进度供客户端播放速率回归使用。
    // 必须与 collect_and_log 解耦，否则按 3s 诊断周期采样时 slope 窗口内
    // 只有 1-2 个样本点，线性回归无意义（slope_s 始终为 0）。
    void record_rb_occupancy();

    // 低频（~3s，DIAGNOSTICS_REFRESH_INTERVAL）：采集 JitterBuffer + RingBuffer
    // 全部指标，生成快照并输出日志。需要外部周期调用，周期：通常 3s。
    void collect_and_log(const jitter::JitterBuffer& jb);

    // ---- 诊断快照 ----

    struct Snapshot {
        // Network
        double rtt_ms = 0.0;
        double interarrival_jitter_ms = 0.0;
        std::uint64_t packets_received = 0;
        std::uint64_t packets_lost = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t late_packets = 0;
        std::uint64_t jb_malformed_packets = 0; // payload 大小不匹配的畸形包（JB 丢弃）
        std::uint64_t recv_audio_bytes = 0; // 收到的音频总字节数（payload only）
        std::uint64_t recv_hello_acks = 0; // 收到的 HELLO_ACK 总数

        // JitterBuffer
        std::size_t jb_current_packets = 0;
        double jb_current_ms = 0.0;
        double jb_avg_ms = 0.0;
        double jb_min_ms = 0.0;
        double jb_max_ms = 0.0;
        double jb_capacity_ms = 0.0;
        double jb_target_ms = 0.0; // 当前自适应 target（客户端恒启用；库固定模式 = floor）

        // RingBuffer
        double rb_current_ms = 0.0;
        double rb_avg_ms = 0.0;
        double rb_min_ms = 0.0;
        double rb_max_ms = 0.0;
        double rb_capacity_ms = 0.0;
        std::uint64_t underruns = 0;
        std::uint64_t deadline_misses = 0;
        std::uint64_t rb_rearms = 0; // pre-roll latch 重臂次数（饥饿 + 看门狗）

        // Buffer occupancy slope (experimental, not clock drift)
        double short_slope_samples_per_s = 0.0;
        double long_slope_samples_per_s = 0.0;

        // 端到端延迟（当前缓冲量 = JB + RB，无需时间同步）
        double end_to_end_ms = 0.0;
        // 时钟漂移（server 发送速率 vs 客户端播放速率的偏差，ppm）
        double drift_ppm = 0.0;
    };

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        return last_snapshot_;
    }

private:
    std::uint32_t sample_rate_;
    std::size_t frame_bytes_;
    std::size_t payload_size_;
    RingBufferFillFn rb_fill_fn_;
    std::size_t rb_capacity_bytes_;
    PlayedSamplesFn played_samples_fn_;

    // RTT 测量。record_hello_sent/record_hello_ack_received 与 collect_and_log 跨线程访问，
    // 用 relaxed atomic 容忍读到旧值（诊断数据不要求精确）。
    std::atomic<std::int64_t> last_hello_sent_ns_ { 0 };
    std::atomic<double> rtt_smoothed_ms_ { 0.0 };

    // Interarrival jitter (RFC 3550 EWMA)
    bool first_packet_ = true;
    std::uint32_t last_seq_ = 0;
    std::uint32_t last_sample_pos_ = 0;
    std::chrono::steady_clock::time_point last_arrival_ { };
    std::atomic<double> jitter_ms_ { 0.0 };

    // end_to_end_ms 与 drift_ppm 由主线程 collect_and_log 计算，
    // 无需跨线程 atomic（回归数据在主线程维护）。

    // RingBuffer occupancy 历史采样（用于 slope 计算）
    struct TimeSample {
        std::chrono::steady_clock::time_point time;
        double value; // 语义随窗口而定：ms / sample_position / 播放帧数
    };
    std::deque<TimeSample> short_window_; // ~5s，value = RB 占用 ms
    std::deque<TimeSample> long_window_; // ~60s，value = RB 占用 ms

    // server 发送速率回归：包到达 (时间, sample_position)
    // 由 io_context 线程写入、主线程读取，用 mutex 保护。
    std::deque<TimeSample> arrival_history_; // ~10s，value = 累积 sample_position（帧）
    mutable std::mutex arrival_mutex_;
    std::int64_t arrival_pos_accum_ = 0; // 累积 sample_position（int32 差值累加，避免 uint32 回绕）
    // 客户端播放速率回归：(时间, 累计播放帧数)，仅主线程访问
    std::deque<TimeSample> played_history_; // ~10s，value = 播放帧数

    // JitterBuffer occupancy 历史采样（用于 min/max/avg）
    std::deque<double> jb_occupancy_history_ms_;
    std::deque<double> rb_occupancy_history_ms_;

    // 计数器（跨线程，relaxed atomic）
    std::atomic<std::uint64_t> underruns_ { 0 };
    std::atomic<std::uint64_t> deadline_misses_ { 0 };
    std::atomic<std::uint64_t> rb_rearms_ { 0 };
    std::atomic<std::uint64_t> recv_audio_bytes_ { 0 };
    std::atomic<std::uint64_t> recv_hello_acks_ { 0 };

    // 上次快照（collect_and_log 写、snapshot 读，跨线程需保护）
    Snapshot last_snapshot_;
    mutable std::mutex snapshot_mutex_;

    static constexpr auto SHORT_WINDOW = std::chrono::seconds(5);
    static constexpr auto LONG_WINDOW = std::chrono::seconds(60);
    static constexpr auto RATE_WINDOW = std::chrono::seconds(10); // 速率回归窗口
    static constexpr std::size_t MAX_HISTORY = 100; // occupancy 历史上限
    static constexpr std::size_t MAX_RATE_HISTORY = 200; // 速率回归历史上限（避免锁内 O(n) 过久）

    double bytes_to_ms(std::size_t bytes) const noexcept;

    double packets_to_ms(std::size_t packets) const noexcept;

    void prune_samples(std::deque<TimeSample>& samples,
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::duration max_age);
    // 线性回归斜率（value 单位 / 秒）。少于 2 个点返回 0。
    double regression_slope(const std::deque<TimeSample>& samples) const;
};

} // namespace aqua::diag

#endif // AQUA_DIAGNOSTICS_MANAGER_H
