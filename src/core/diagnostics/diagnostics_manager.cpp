#include "core/diagnostics/diagnostics_manager.h"
#include "core/logger/logger.h"

#include <algorithm>
#include <cmath>

namespace aqua::diag {

DiagnosticsManager::DiagnosticsManager(std::uint32_t sample_rate,
    std::size_t frame_bytes,
    std::size_t payload_size,
    RingBufferFillFn rb_fill_fn,
    std::size_t rb_capacity_bytes,
    PlayedSamplesFn played_samples_fn)
    : sample_rate_(sample_rate)
    , frame_bytes_(frame_bytes)
    , payload_size_(payload_size)
    , rb_fill_fn_(std::move(rb_fill_fn))
    , rb_capacity_bytes_(rb_capacity_bytes)
    , played_samples_fn_(std::move(played_samples_fn))
{
}

void DiagnosticsManager::record_packet_arrival(std::uint32_t sequence,
    std::uint32_t sample_position)
{
    auto now = std::chrono::steady_clock::now();

    if (first_packet_) {
        first_packet_ = false;
        last_seq_ = sequence;
        last_sample_pos_ = sample_position;
        last_arrival_ = now;
        arrival_pos_accum_ = 0; // 以首包为基准
        // 首包无前包可比较，仅入队供回归使用，不参与 interarrival jitter。
        {
            std::lock_guard<std::mutex> lock(arrival_mutex_);
            arrival_history_.push_back({ now, 0.0 });
            prune_samples(arrival_history_, now, RATE_WINDOW);
            while (arrival_history_.size() > MAX_RATE_HISTORY)
                arrival_history_.pop_front();
        }
        return;
    }

    // RFC 3550 interarrival jitter
    // D = (arrival_j - arrival_i) - (sample_pos_j - sample_pos_i) / sample_rate
    auto arrival_delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - last_arrival_)
                                .count();
    double arrival_delta_s = static_cast<double>(arrival_delta_ns) / 1e9;

    // sample_position 差值（处理 uint32 回绕）。
    // 利用 uint32 减法回绕 + int32 有符号转换，与 JitterBuffer::seq_diff 同一手法。
    // 例如 0xFFFFFFFF → 0 时，(0 - 0xFFFFFFFF) as int32 = 1，正确表示前进了 1 帧。
    std::int32_t sp_diff = static_cast<std::int32_t>(sample_position - last_sample_pos_);
    double sp_delta_s = static_cast<double>(sp_diff) / sample_rate_;

    double d = arrival_delta_s - sp_delta_s;
    double d_ms = std::abs(d) * 1000.0;

    // EWMA: J = J + (|D| - J) / 16
    double j = jitter_ms_.load(std::memory_order_relaxed);
    j += (d_ms - j) / 16.0;
    jitter_ms_.store(j, std::memory_order_relaxed);

    // 采样 (到达时间, 累积 sample_position) 供 server 发送速率回归。
    // arrival_history_ 由 io_context 线程（本函数）写入、主线程（collect_and_log）
    // 读取，用 arrival_mutex_ 保护。
    // 用 int64 累积值避免 uint32 sample_position 回绕导致回归跳变。
    arrival_pos_accum_ += sp_diff;
    {
        std::lock_guard<std::mutex> lock(arrival_mutex_);
        arrival_history_.push_back({ now, static_cast<double>(arrival_pos_accum_) });
        prune_samples(arrival_history_, now, RATE_WINDOW);
        while (arrival_history_.size() > MAX_RATE_HISTORY)
            arrival_history_.pop_front();
    }

    last_seq_ = sequence;
    last_sample_pos_ = sample_position;
    last_arrival_ = now;
}

void DiagnosticsManager::record_hello_sent()
{
    last_hello_sent_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);
}

void DiagnosticsManager::record_hello_ack_received()
{
    auto sent = last_hello_sent_ns_.load(std::memory_order_relaxed);
    if (sent == 0)
        return;

    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    auto delta_us = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
        std::chrono::nanoseconds(now_ns - sent));
    double rtt_ms = delta_us.count() / 1000.0;

    // EWMA 平滑
    double smoothed = rtt_smoothed_ms_.load(std::memory_order_relaxed);
    if (smoothed == 0.0) {
        smoothed = rtt_ms;
    } else {
        smoothed = 0.8 * smoothed + 0.2 * rtt_ms;
    }
    rtt_smoothed_ms_.store(smoothed, std::memory_order_relaxed);
}

void DiagnosticsManager::record_underrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }

void DiagnosticsManager::record_deadline_miss() { deadline_misses_.fetch_add(1, std::memory_order_relaxed); }

void DiagnosticsManager::record_rb_rearm() { rb_rearms_.fetch_add(1, std::memory_order_relaxed); }

void DiagnosticsManager::record_audio_bytes(std::size_t bytes) { recv_audio_bytes_.fetch_add(bytes, std::memory_order_relaxed); }

void DiagnosticsManager::record_hello_ack() { recv_hello_acks_.fetch_add(1, std::memory_order_relaxed); }

void DiagnosticsManager::record_rb_occupancy()
{
    auto now = std::chrono::steady_clock::now();
    std::size_t rb_bytes = rb_fill_fn_ ? rb_fill_fn_() : 0;
    double rb_ms = bytes_to_ms(rb_bytes);

    short_window_.push_back({ now, rb_ms });
    long_window_.push_back({ now, rb_ms });
    prune_samples(short_window_, now, SHORT_WINDOW);
    prune_samples(long_window_, now, LONG_WINDOW);

    // 采样 (时间, 累计播放帧数) 供客户端播放速率回归。
    // played_samples_fn_ 读取的是播放线程累加的 atomic，主线程读无并发问题。
    if (played_samples_fn_) {
        played_history_.push_back({ now,
            static_cast<double>(played_samples_fn_()) });
        prune_samples(played_history_, now, RATE_WINDOW);
    }
}

void DiagnosticsManager::collect_and_log(const jitter::JitterBuffer& jb)
{
    // JitterBuffer 指标
    std::size_t jb_fill = jb.buffer_fill_packets();
    double jb_ms = packets_to_ms(jb_fill);

    // RingBuffer 指标（当前快照，不复用 slope 窗口）
    std::size_t rb_bytes = rb_fill_fn_ ? rb_fill_fn_() : 0;
    double rb_ms = bytes_to_ms(rb_bytes);

    // 记录历史（用于 min/max/avg）
    jb_occupancy_history_ms_.push_back(jb_ms);
    rb_occupancy_history_ms_.push_back(rb_ms);
    if (jb_occupancy_history_ms_.size() > MAX_HISTORY)
        jb_occupancy_history_ms_.pop_front();
    if (rb_occupancy_history_ms_.size() > MAX_HISTORY)
        rb_occupancy_history_ms_.pop_front();

    // 计算 slope（short_window_ 由 record_rb_occupancy() 高频填充）
    // 值单位为 ms，回归斜率需换算为 samples/s。
    // arrival_history_ 回归：先 copy 出 deque（持锁），再在锁外计算避免阻塞 io_context 线程。
    double short_slope = regression_slope(short_window_) * sample_rate_ / 1000.0;
    double long_slope = regression_slope(long_window_) * sample_rate_ / 1000.0;
    double server_rate = 0.0;
    {
        std::lock_guard<std::mutex> lock(arrival_mutex_);
        server_rate = regression_slope(arrival_history_);
    }

    // 计算 min/max/avg
    auto stats = [](const std::deque<double>& hist) -> std::tuple<double, double, double> {
        if (hist.empty())
            return { 0.0, 0.0, 0.0 };
        double sum = 0.0, mn = hist[0], mx = hist[0];
        for (auto v : hist) {
            sum += v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        return { sum / hist.size(), mn, mx };
    };

    auto [jb_avg, jb_min, jb_max] = stats(jb_occupancy_history_ms_);
    auto [rb_avg, rb_min, rb_max] = stats(rb_occupancy_history_ms_);

    // 构建快照。锁内填局部 snap，并同步一份到 last_snapshot_（供 snapshot() 跨线程读），
    // 锁外直接用 snap 输出日志，避免 snapshot() 的二次加锁拷贝。
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        auto& s = snap;
        s.rtt_ms = rtt_smoothed_ms_.load(std::memory_order_relaxed);
        s.interarrival_jitter_ms = jitter_ms_.load(std::memory_order_relaxed);
        s.packets_received = jb.packets_received();
        s.packets_lost = jb.packets_lost();
        s.duplicates = jb.duplicates();
        s.late_packets = jb.late_packets();
        s.jb_malformed_packets = jb.malformed_packets();
        s.jb_current_packets = jb_fill;
        s.jb_current_ms = jb_ms;
        s.jb_avg_ms = jb_avg;
        s.jb_min_ms = jb_min;
        s.jb_max_ms = jb_max;
        s.jb_capacity_ms = packets_to_ms(jb.capacity_packets());
        s.jb_target_ms = packets_to_ms(jb.target_latency_packets());
        s.rb_current_ms = rb_ms;
        s.rb_avg_ms = rb_avg;
        s.rb_min_ms = rb_min;
        s.rb_max_ms = rb_max;
        s.rb_capacity_ms = bytes_to_ms(rb_capacity_bytes_);
        s.underruns = underruns_.load(std::memory_order_relaxed);
        s.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
        s.rb_rearms = rb_rearms_.load(std::memory_order_relaxed);
        s.recv_audio_bytes = recv_audio_bytes_.load(std::memory_order_relaxed);
        s.recv_hello_acks = recv_hello_acks_.load(std::memory_order_relaxed);
        s.short_slope_samples_per_s = short_slope;
        s.long_slope_samples_per_s = long_slope;

        // 端到端延迟 + 时钟漂移
        // 端到端延迟：当前缓冲量（JB + RB），无需时间同步，语义即"此刻的缓冲延迟"。
        s.end_to_end_ms = jb_ms + rb_ms;

        // 时钟漂移：server 发送速率 vs 客户端播放速率的偏差（ppm）。
        // 两者都用最近 RATE_WINDOW 内的线性回归斜率（帧/秒），
        // 回归平均掉逐包抖动和包边界相位，得到稳定、单一符号的真实速率差。
        // 正 = server 快于播放（JB 渐满），负 = server 慢于播放（JB 渐空）。
        double client_rate = regression_slope(played_history_);
        if (server_rate > 0.0 && client_rate > 0.0) {
            s.drift_ppm = (server_rate / client_rate - 1.0) * 1e6;
        } else {
            s.drift_ppm = 0.0;
        }
        last_snapshot_ = snap;
    }

    std::uint64_t total_lost = snap.packets_lost + snap.late_packets;
    double loss_rate = (snap.packets_received > 0)
        ? static_cast<double>(total_lost) * 100.0 / snap.packets_received
        : 0.0;

    aqua::log_debug_fmt(
        "Client diag: RTT={:.1f}ms jitter={:.2f}ms loss={}/{:.3f}% dup={} late={} malformed={} dmiss={} "
        "JB[{:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}ms target={:.0f}ms] "
        "RB[{:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}ms] "
        "underrun={} rearm={} slope_s={:.1f} slope_l={:.1f} e2e={:.1f}ms drift={:.1f}ppm "
        "rx_bytes={} acks={}",
        snap.rtt_ms, snap.interarrival_jitter_ms,
        total_lost, loss_rate, snap.duplicates, snap.late_packets, snap.jb_malformed_packets,
        snap.deadline_misses,
        snap.jb_current_ms, snap.jb_avg_ms, snap.jb_min_ms, snap.jb_max_ms, snap.jb_capacity_ms,
        snap.jb_target_ms,
        snap.rb_current_ms, snap.rb_avg_ms, snap.rb_min_ms, snap.rb_max_ms, snap.rb_capacity_ms,
        snap.underruns, snap.rb_rearms, snap.short_slope_samples_per_s, snap.long_slope_samples_per_s,
        snap.end_to_end_ms, snap.drift_ppm,
        snap.recv_audio_bytes, snap.recv_hello_acks);
}

double DiagnosticsManager::bytes_to_ms(std::size_t bytes) const noexcept
{
    if (frame_bytes_ == 0 || sample_rate_ == 0)
        return 0.0;
    double frames = static_cast<double>(bytes) / frame_bytes_;
    return frames * 1000.0 / sample_rate_;
}

double DiagnosticsManager::packets_to_ms(std::size_t packets) const noexcept
{
    if (frame_bytes_ == 0 || sample_rate_ == 0)
        return 0.0;
    // packet_duration = frames_per_packet / sample_rate * 1000
    double frames_per_packet = static_cast<double>(payload_size_) / frame_bytes_;
    return static_cast<double>(packets) * frames_per_packet * 1000.0 / sample_rate_;
}

void DiagnosticsManager::prune_samples(std::deque<TimeSample>& samples,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration max_age)
{
    while (!samples.empty() && now - samples.front().time > max_age) {
        samples.pop_front();
    }
}

double DiagnosticsManager::regression_slope(const std::deque<TimeSample>& samples) const
{
    if (samples.size() < 2)
        return 0.0;

    // 线性回归：y = a + b*x，返回 b（value 单位 / 秒）
    // x = 时间（秒），y = value
    auto t0 = samples.front().time;
    double n = static_cast<double>(samples.size());
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

    for (const auto& s : samples) {
        double x = std::chrono::duration_cast<std::chrono::duration<double>>(s.time - t0).count();
        double y = s.value;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-12)
        return 0.0;

    return (n * sum_xy - sum_x * sum_y) / denom; // value 单位 / 秒
}

} // namespace aqua::diag
