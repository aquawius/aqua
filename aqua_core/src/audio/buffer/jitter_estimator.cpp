#include "aqua/audio/buffer/jitter_estimator.h"

#include "aqua/net/udp/network_frame.h"

namespace aqua::audio {

namespace {

    constexpr double kNsPerMs = 1000000.0;
    constexpr double kJitterGain = 1.0 / 16.0; // RFC 3550 A.8 增益

} // namespace

JitterEstimator::JitterEstimator(std::uint32_t timestamp_rate_hz, std::uint32_t frames_per_packet,
    double stall_threshold_packet_periods) noexcept
    : timestamp_rate_hz_(
          timestamp_rate_hz > 0 && frames_per_packet > 0 ? static_cast<double>(timestamp_rate_hz) : 1.0)
    , packet_ms_(timestamp_rate_hz > 0 && frames_per_packet > 0
              ? static_cast<double>(frames_per_packet) * 1000.0 / timestamp_rate_hz_
              : 0.0)
    , stall_threshold_ms_(
          // 阈值必须严格大于 burst 串间间隔（≈2.7 包周期），否则会把正常发包
          // 误当 stall；<= 0 视为关闭（不做 stall 剔除）。
          stall_threshold_packet_periods > 0.0 && packet_ms_ > 0.0
              ? stall_threshold_packet_periods * packet_ms_
              : 0.0)
{
}

void JitterEstimator::reset() noexcept
{
    have_packets_ = false;
    current_ssrc_ = 0;
    max_ext_ = 0;
    window_bits_ = 0;
    last_timestamp_ = 0;
    last_arrival_ns_ = 0;
    anchor_timestamp_ = 0;
    anchor_arrival_ns_ = 0;
    jitter_ms_ = 0.0;
    base_delay_ms_ = 0.0;
    transit_ms_.store(0.0, std::memory_order_relaxed);
    jitter_ms_out_.store(0.0, std::memory_order_relaxed);
    base_delay_ms_out_.store(0.0, std::memory_order_relaxed);
    arrival_interval_ms_.store(0.0, std::memory_order_relaxed);
    packets_.store(0, std::memory_order_relaxed);
    duplicates_.store(0, std::memory_order_relaxed);
    reordered_.store(0, std::memory_order_relaxed);
    late_.store(0, std::memory_order_relaxed);
    gap_events_.store(0, std::memory_order_relaxed);
    missing_packets_.store(0, std::memory_order_relaxed);
    stall_events_.store(0, std::memory_order_relaxed);
    last_stall_gap_ms_.store(0.0, std::memory_order_relaxed);
}

void JitterEstimator::observe(std::uint16_t seq, std::uint32_t timestamp, std::uint32_t ssrc,
    std::int64_t arrival_ns) noexcept
{
    // SSRC 变化 = 新流：旧时间轴作废，全重置后按首包处理（JB 侧 SSRC 钉住同模型）。
    if (have_packets_ && ssrc != current_ssrc_) {
        reset();
    }
    packets_.fetch_add(1, std::memory_order_relaxed);

    if (!have_packets_) {
        have_packets_ = true;
        current_ssrc_ = ssrc;
        max_ext_ = seq;
        window_bits_ = 1; // bit0 = max 本身已见
        last_timestamp_ = timestamp;
        last_arrival_ns_ = arrival_ns;
        anchor_timestamp_ = timestamp;
        anchor_arrival_ns_ = arrival_ns;
        return; // 首包只建基线：无 transit/jitter（相对量无参照）。
    }

    const std::uint64_t ext = net::extend_rtp_sequence(max_ext_, seq);

    // ---- seq 分类（与 JB 接受与否无关，观测的是网络）----
    if (ext > max_ext_) {
        if (ext > max_ext_ + 1) {
            gap_events_.fetch_add(1, std::memory_order_relaxed);
            missing_packets_.fetch_add(ext - max_ext_ - 1, std::memory_order_relaxed);
        }
        const std::uint64_t shift = ext - max_ext_;
        max_ext_ = ext;
        // 窗口前移：老位丢弃（shift >= 64 则窗口全空）。
        window_bits_ = (shift >= kEstimatorReorderWindow) ? 0 : (window_bits_ << shift);
        window_bits_ |= 1;
    } else {
        const std::uint64_t behind = max_ext_ - ext;
        if (behind < kEstimatorReorderWindow && (window_bits_ & (std::uint64_t { 1 } << behind)) != 0) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
        } else if (behind < kEstimatorReorderWindow) {
            reordered_.fetch_add(1, std::memory_order_relaxed);
            window_bits_ |= (std::uint64_t { 1 } << behind);
        } else {
            late_.fetch_add(1, std::memory_order_relaxed);
        }
        // 落后包不更新 transit/jitter/到达基线（RFC 口径：只对按序到达做 J；
        // 且落后包的到达间隔为负，会污染统计）。
        return;
    }

    // ---- 时间统计（只对 max 前进的包）----
    const double arrival_interval_ms
        = static_cast<double>(arrival_ns - last_arrival_ns_) / kNsPerMs;
    arrival_interval_ms_.store(arrival_interval_ms, std::memory_order_relaxed);
    last_arrival_ns_ = arrival_ns;

    // timestamp 差用 u32 模运算（回绕安全）；非单调（dts <= 0）= 发送端时间轴
    // 重置：重建 transit 锚点并跳过本次 J 更新，不触发任何 JB 动作。
    const std::int32_t delta_ts = static_cast<std::int32_t>(timestamp - last_timestamp_);
    last_timestamp_ = timestamp;
    if (delta_ts <= 0) {
        anchor_timestamp_ = timestamp;
        anchor_arrival_ns_ = arrival_ns;
        return;
    }
    const double sender_ms = static_cast<double>(delta_ts) * 1000.0 / timestamp_rate_hz_;
    const double diff_ms = arrival_interval_ms - sender_ms;

    // stall（时间断流）与抖动分离：到达间隔超过阈值（默认 5 个包周期 ≈
    // 18.75ms）就不是"抖动"而是断流/严重拥塞。把它喂进 RFC 3550 的 J 会让
    // J 瞬间飙升数倍、target 过冲后按跌侧限速花十几秒才回落——双机实测一次
    // 170ms 的 Wi-Fi stall 把 target 从 7 顶到 21 挂了 14s，且 stall 的 45
    // 个滞后包随后 burst 回补把 30 槽环形打爆（busy 拒绝 + 4 次 reanchor）。
    // 断流该由欠载反馈（penalty）和 reanchor 负责，不归 J 管（NetEq 的
    // DelayManager 同样把长延迟/间隙交给 peak handling 而非 IAT 均值）。
    // 注意：transit/base 仍照常更新——它们是"这包多晚"的绝对量，不污染。
    const bool is_stall
        = stall_threshold_ms_ > 0.0 && arrival_interval_ms > stall_threshold_ms_;
    if (is_stall) {
        stall_events_.fetch_add(1, std::memory_order_relaxed);
        last_stall_gap_ms_.store(arrival_interval_ms, std::memory_order_relaxed);
    } else {
        // RFC 3550 A.8：J += (|D| - J) / 16。stall 样本不入统计。
        const double abs_d = diff_ms >= 0.0 ? diff_ms : -diff_ms;
        jitter_ms_ += (abs_d - jitter_ms_) * kJitterGain;
        jitter_ms_out_.store(jitter_ms_, std::memory_order_relaxed);
    }

    // 相对 transit：锚点差分，timestamp 随机 offset 消去。
    const double transit_ms = static_cast<double>(arrival_ns - anchor_arrival_ns_) / kNsPerMs
        - static_cast<double>(static_cast<std::int32_t>(timestamp - anchor_timestamp_)) * 1000.0
            / timestamp_rate_hz_;
    transit_ms_.store(transit_ms, std::memory_order_relaxed);
    if (transit_ms < base_delay_ms_ || base_delay_ms_ == 0.0) {
        // 累积最小值 = 路径底噪。注意：这是 Phase 0 的简化口径（长期单调漂移
        // 下底噪只会偏低不会偏高；Phase 1 视数据换窗口最小值）。
        // 首个有效 transit 直接采用（base == 0 哨兵）。
        base_delay_ms_ = transit_ms;
        base_delay_ms_out_.store(base_delay_ms_, std::memory_order_relaxed);
    }
}

JitterEstimates JitterEstimator::estimates() const noexcept
{
    JitterEstimates out;
    out.transit_ms = transit_ms_.load(std::memory_order_relaxed);
    out.jitter_ms = jitter_ms_out_.load(std::memory_order_relaxed);
    out.base_delay_ms = base_delay_ms_out_.load(std::memory_order_relaxed);
    out.arrival_interval_ms = arrival_interval_ms_.load(std::memory_order_relaxed);
    out.packets = packets_.load(std::memory_order_relaxed);
    out.duplicates = duplicates_.load(std::memory_order_relaxed);
    out.reordered = reordered_.load(std::memory_order_relaxed);
    out.late = late_.load(std::memory_order_relaxed);
    out.gap_events = gap_events_.load(std::memory_order_relaxed);
    out.missing_packets = missing_packets_.load(std::memory_order_relaxed);
    out.stall_events = stall_events_.load(std::memory_order_relaxed);
    out.last_stall_gap_ms = last_stall_gap_ms_.load(std::memory_order_relaxed);
    return out;
}

} // namespace aqua::audio
