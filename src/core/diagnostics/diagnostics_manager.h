#ifndef AQUA_DIAGNOSTICS_MANAGER_H
#define AQUA_DIAGNOSTICS_MANAGER_H

#include "core/jitter_buffer/jitter_buffer.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>

namespace aqua::diag {

// DiagnosticsManager：M5 诊断数据采集与输出。
//
// 从 JitterBuffer 和 RingBuffer 采集指标，计算 interarrival jitter、
// RingBuffer occupancy slope（short/long 窗口），周期性输出日志。
//
// 所有方法在 client 主线程调用（非热路径），不影响音频管线。
class DiagnosticsManager {
public:
    // 采样回调：返回当前 RingBuffer available_read 字节数
    using RingBufferFillFn = std::function<std::size_t()>;

    DiagnosticsManager(std::uint32_t sample_rate,
                       std::size_t frame_bytes,
                       std::size_t payload_size,
                       RingBufferFillFn rb_fill_fn);

    // 在 UDP 回调中调用（io_context 线程），记录 packet 到达
    void on_packet_received(std::uint32_t sequence, std::uint32_t sample_position);

    // 在 HELLO 发送/HELLO_ACK 接收时调用，记录 RTT
    void on_hello_sent();
    void on_hello_ack_received();

    // 在 WASAPI playback 回调返回不足时调用
    void on_underrun() { ++underruns_; }

    // 在 JB timer 调度延迟超过 1 个 packet_duration 时调用
    void on_deadline_miss() { ++deadline_misses_; }

    // 主线程高频调用（~50ms）：仅采样 RingBuffer 占用到 slope 窗口。
    // 必须与 sample_and_log 解耦，否则 5s 窗口只有 1-2 个样本点，
    // 线性回归无意义（slope_s 始终为 0）。
    void sample_ringbuffer();

    // 主线程低频调用（~5s）：采集 JitterBuffer + RingBuffer 指标，输出日志
    // interval: 调用周期（通常 5s）
    void sample_and_log(const jitter::JitterBuffer& jb,
                        std::chrono::steady_clock::duration interval);

    // ---- 诊断快照 ----

    struct Snapshot {
        // Network
        double rtt_ms = 0.0;
        double interarrival_jitter_ms = 0.0;
        std::uint64_t packets_received = 0;
        std::uint64_t packets_lost = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t late_packets = 0;

        // JitterBuffer
        std::size_t jb_target_packets = 0;
        std::size_t jb_current_packets = 0;
        double jb_current_ms = 0.0;
        double jb_avg_ms = 0.0;
        double jb_min_ms = 0.0;
        double jb_max_ms = 0.0;

        // RingBuffer
        double rb_current_ms = 0.0;
        double rb_avg_ms = 0.0;
        double rb_min_ms = 0.0;
        double rb_max_ms = 0.0;
        std::uint64_t underruns = 0;
        std::uint64_t deadline_misses = 0;

        // Buffer occupancy slope (experimental, not clock drift)
        double short_slope_samples_per_s = 0.0;
        double long_slope_samples_per_s = 0.0;
    };

    Snapshot snapshot() const { return last_snapshot_; }

private:
    std::uint32_t sample_rate_;
    std::size_t frame_bytes_;
    std::size_t payload_size_;
    RingBufferFillFn rb_fill_fn_;

    // RTT 测量
    std::chrono::steady_clock::time_point last_hello_sent_{};
    double rtt_ms_ = 0.0;
    double rtt_smoothed_ms_ = 0.0;

    // Interarrival jitter (RFC 3550 EWMA)
    bool first_packet_ = true;
    std::uint32_t last_seq_ = 0;
    std::uint32_t last_sample_pos_ = 0;
    std::chrono::steady_clock::time_point last_arrival_{};
    double jitter_ms_ = 0.0;

    // RingBuffer occupancy 历史采样（用于 slope 计算）
    struct OccupancySample {
        std::chrono::steady_clock::time_point time;
        double ms;  // occupancy in ms
    };
    std::deque<OccupancySample> short_window_;  // ~5s
    std::deque<OccupancySample> long_window_;   // ~60s

    // JitterBuffer occupancy 历史采样（用于 min/max/avg）
    std::deque<double> jb_occupancy_history_ms_;
    std::deque<double> rb_occupancy_history_ms_;

    // 计数器
    std::uint64_t underruns_ = 0;
    std::uint64_t deadline_misses_ = 0;

    // 上次快照
    Snapshot last_snapshot_;

    // 上次采集时间
    std::chrono::steady_clock::time_point last_sample_time_{};

    static constexpr auto SHORT_WINDOW = std::chrono::seconds(5);
    static constexpr auto LONG_WINDOW = std::chrono::seconds(60);
    static constexpr std::size_t MAX_HISTORY = 100;

    double bytes_to_ms(std::size_t bytes) const noexcept {
        if (frame_bytes_ == 0 || sample_rate_ == 0) return 0.0;
        double frames = static_cast<double>(bytes) / frame_bytes_;
        return frames * 1000.0 / sample_rate_;
    }

    double packets_to_ms(std::size_t packets) const noexcept {
        // packet_duration = frames_per_packet / sample_rate * 1000
        double frames_per_packet = static_cast<double>(payload_size_) / frame_bytes_;
        return static_cast<double>(packets) * frames_per_packet * 1000.0 / sample_rate_;
    }

    void prune_windows(std::chrono::steady_clock::time_point now);
    double compute_slope(const std::deque<OccupancySample>& window) const;
};

} // namespace aqua::diag

#endif // AQUA_DIAGNOSTICS_MANAGER_H
