#include "core/diagnostics/diagnostics_manager.h"
#include "core/logger/logger.h"

#include <algorithm>
#include <cmath>

namespace aqua::diag {

DiagnosticsManager::DiagnosticsManager(std::uint32_t sample_rate,
                                       std::size_t frame_bytes,
                                       std::size_t payload_size,
                                       RingBufferFillFn rb_fill_fn)
    : sample_rate_(sample_rate)
    , frame_bytes_(frame_bytes)
    , payload_size_(payload_size)
    , rb_fill_fn_(std::move(rb_fill_fn))
{
}

void DiagnosticsManager::on_packet_received(std::uint32_t sequence,
                                            std::uint32_t sample_position)
{
    auto now = std::chrono::steady_clock::now();

    if (first_packet_) {
        first_packet_ = false;
        last_seq_ = sequence;
        last_sample_pos_ = sample_position;
        last_arrival_ = now;
        return;
    }

    // RFC 3550 interarrival jitter
    // D = (arrival_j - arrival_i) - (sample_pos_j - sample_pos_i) / sample_rate
    auto arrival_delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - last_arrival_).count();
    double arrival_delta_s = static_cast<double>(arrival_delta_ns) / 1e9;

    // sample_position 差值（处理 uint32 回绕）
    std::int64_t sp_diff = static_cast<std::int64_t>(sample_position) -
                           static_cast<std::int64_t>(last_sample_pos_);
    double sp_delta_s = static_cast<double>(sp_diff) / sample_rate_;

    double d = arrival_delta_s - sp_delta_s;
    double d_ms = std::abs(d) * 1000.0;

    // EWMA: J = J + (|D| - J) / 16
    jitter_ms_ += (d_ms - jitter_ms_) / 16.0;

    last_seq_ = sequence;
    last_sample_pos_ = sample_position;
    last_arrival_ = now;
}

void DiagnosticsManager::on_hello_sent()
{
    last_hello_sent_ = std::chrono::steady_clock::now();
}

void DiagnosticsManager::on_hello_ack_received()
{
    if (last_hello_sent_.time_since_epoch().count() == 0) return;

    auto now = std::chrono::steady_clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - last_hello_sent_);
    rtt_ms_ = static_cast<double>(delta.count()) / 1000.0;

    // EWMA 平滑
    if (rtt_smoothed_ms_ == 0.0) {
        rtt_smoothed_ms_ = rtt_ms_;
    } else {
        rtt_smoothed_ms_ = 0.8 * rtt_smoothed_ms_ + 0.2 * rtt_ms_;
    }
}

void DiagnosticsManager::sample_and_log(const jitter::JitterBuffer& jb,
                                        std::chrono::steady_clock::duration interval)
{
    auto now = std::chrono::steady_clock::now();

    // JitterBuffer 指标
    std::size_t jb_fill = jb.buffer_fill_packets();
    double jb_ms = packets_to_ms(jb_fill);

    // RingBuffer 指标
    std::size_t rb_bytes = rb_fill_fn_ ? rb_fill_fn_() : 0;
    double rb_ms = bytes_to_ms(rb_bytes);

    // 记录历史
    jb_occupancy_history_ms_.push_back(jb_ms);
    rb_occupancy_history_ms_.push_back(rb_ms);
    if (jb_occupancy_history_ms_.size() > MAX_HISTORY) jb_occupancy_history_ms_.pop_front();
    if (rb_occupancy_history_ms_.size() > MAX_HISTORY) rb_occupancy_history_ms_.pop_front();

    // RingBuffer occupancy slope 采样
    short_window_.push_back({now, rb_ms});
    long_window_.push_back({now, rb_ms});
    prune_windows(now);

    // 计算 slope
    double short_slope = compute_slope(short_window_);
    double long_slope = compute_slope(long_window_);

    // 计算 min/max/avg
    auto stats = [](const std::deque<double>& hist) -> std::tuple<double, double, double> {
        if (hist.empty()) return {0.0, 0.0, 0.0};
        double sum = 0.0, mn = hist[0], mx = hist[0];
        for (auto v : hist) {
            sum += v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        return {sum / hist.size(), mn, mx};
    };

    auto [jb_avg, jb_min, jb_max] = stats(jb_occupancy_history_ms_);
    auto [rb_avg, rb_min, rb_max] = stats(rb_occupancy_history_ms_);

    // 构建快照
    auto& s = last_snapshot_;
    s.rtt_ms = rtt_smoothed_ms_;
    s.interarrival_jitter_ms = jitter_ms_;
    s.packets_received = jb.packets_received();
    s.packets_lost = jb.packets_lost();
    s.duplicates = jb.duplicates();
    s.late_packets = jb.late_packets();
    s.jb_target_packets = 0; // 不再从 jb 获取 target
    s.jb_current_packets = jb_fill;
    s.jb_current_ms = jb_ms;
    s.jb_avg_ms = jb_avg;
    s.jb_min_ms = jb_min;
    s.jb_max_ms = jb_max;
    s.rb_current_ms = rb_ms;
    s.rb_avg_ms = rb_avg;
    s.rb_min_ms = rb_min;
    s.rb_max_ms = rb_max;
    s.underruns = underruns_;
    s.short_slope_ppm = short_slope;
    s.long_slope_ppm = long_slope;

    // 输出日志
    double interval_s = std::chrono::duration_cast<std::chrono::duration<double>>(interval).count();
    std::uint64_t total_lost = s.packets_lost + s.late_packets;
    double loss_rate = (s.packets_received > 0)
        ? static_cast<double>(total_lost) * 100.0 / s.packets_received
        : 0.0;

    aqua::log_debug_fmt(
        "Client diag: RTT={:.1f}ms jitter={:.2f}ms loss={}/{:.3f}% dup={} late={} "
        "JB[{:.0f}/{:.0f}/{:.0f}/{:.0f}ms] RB[{:.0f}/{:.0f}/{:.0f}/{:.0f}ms] "
        "underrun={} drift_short={:.1f}ppm drift_long={:.1f}ppm",
        s.rtt_ms, s.interarrival_jitter_ms,
        total_lost, loss_rate, s.duplicates, s.late_packets,
        s.jb_current_ms, s.jb_avg_ms, s.jb_min_ms, s.jb_max_ms,
        s.rb_current_ms, s.rb_avg_ms, s.rb_min_ms, s.rb_max_ms,
        s.underruns, s.short_slope_ppm, s.long_slope_ppm);
}

void DiagnosticsManager::prune_windows(std::chrono::steady_clock::time_point now)
{
    while (!short_window_.empty() && now - short_window_.front().time > SHORT_WINDOW) {
        short_window_.pop_front();
    }
    while (!long_window_.empty() && now - long_window_.front().time > LONG_WINDOW) {
        long_window_.pop_front();
    }
}

double DiagnosticsManager::compute_slope(const std::deque<OccupancySample>& window) const
{
    if (window.size() < 2) return 0.0;

    // 线性回归：y = a + b*x，返回 b（ms/s → ppm 转换）
    // x = 时间（秒），y = occupancy（ms）
    auto t0 = window.front().time;
    double n = static_cast<double>(window.size());
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

    for (const auto& s : window) {
        double x = std::chrono::duration_cast<std::chrono::duration<double>>(s.time - t0).count();
        double y = s.ms;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) return 0.0;

    double slope = (n * sum_xy - sum_x * sum_y) / denom;  // ms/s

    // 转换为 ppm：如果平均 occupancy 是 avg_ms，slope 是 ms/s
    // drift_ppm = slope / avg_ms * 1e6
    double avg_y = sum_y / n;
    if (std::abs(avg_y) < 1e-6) return 0.0;

    return slope / avg_y * 1e6;
}

} // namespace aqua::diag
