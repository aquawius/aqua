#include "aqua/audio/buffer/jitter_estimator.h"
#include "aqua/audio/buffer/buffer_config.h"

#include "aqua/net/udp/network_frame.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <cmath>

// 控制面（决策层）日志开关：默认关闭。开启后本编译单元的决策日志会同步调用
// spdlog（内部有锁），组件因此不再满足"无 IO"约束——仅开发期使用。
// 边界：运行在 push strand / 控制线程上的决策与判定；RT 音频回调日志归
// AQUA_CLIENT_RT_DEBUG_LOG。点位全表见 doc/modules/observability.md。
#ifndef AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
#define AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG 0
#endif

namespace aqua::audio {

namespace {

    constexpr double kNsPerMs = 1000000.0;
    // RFC 3550 A.8 EWMA 增益（J 只当诊断，不进控制律——k×J 公式已随 legacy 删除）。
    constexpr double kRfc3550Gain = 1.0 / 16.0;

} // namespace

JitterEstimator::JitterEstimator(std::uint32_t timestamp_rate_hz, std::uint32_t frames_per_packet,
    double stall_threshold_packet_periods, double stall_peak_decay_ms_per_sec) noexcept
    : timestamp_rate_hz_(
          timestamp_rate_hz > 0 && frames_per_packet > 0 ? static_cast<double>(timestamp_rate_hz) : 1.0)
    , packet_ms_(timestamp_rate_hz > 0 && frames_per_packet > 0
              ? static_cast<double>(frames_per_packet) * 1000.0 / timestamp_rate_hz_
              : 0.0)
    , stall_threshold_ms_(
          // 阈值必须显著大于 burst 串间间隔（≈2.7 包周期），否则会把正常发包
          // 误当 stall；<= 0 视为关闭（不做 stall 剔除）。
          stall_threshold_packet_periods > 0.0 && packet_ms_ > 0.0
              ? stall_threshold_packet_periods * packet_ms_
              : 0.0)
    , transit_step_threshold_ms_(packet_ms_ > 0.0
              ? config::JB_TRANSIT_STEP_PACKETS * packet_ms_
              : 0.0)
    // 0 = 峰值永久保持（合法极值，用于"历史最坏间隙"实验）；负值 = 默认值。
    , stall_peak_decay_ms_per_sec_(stall_peak_decay_ms_per_sec >= 0.0
              ? stall_peak_decay_ms_per_sec
              : config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC)
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
    prev_transit_ms_ = 0.0;
    transit_level_ms_ = 0.0;
    transit_level_ms_out_.store(0.0, std::memory_order_relaxed);
    transit_step_events_.store(0, std::memory_order_relaxed);
    last_transit_step_ms_.store(0.0, std::memory_order_relaxed);
    base_delay_ms_ = 0.0;
    base_set_ = false;
    stall_peak_ms_ = 0.0;
    stall_peak_last_ns_ = 0;
    // 尾部直方图清零：ring 内容由 head/count 界定，无需清 16KB；
    // hist 计数必须清，否则旧流尾部污染新流 P99。
    tail_head_ = 0;
    tail_count_ = 0;
    for (auto& c : tail_hist_) {
        c = 0;
    }
    tail_p99_ms_out_.store(-1.0, std::memory_order_relaxed);
    tail_samples_out_.store(0, std::memory_order_relaxed);
    jitter_sample_count_ = 0;
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
    stall_peak_ms_out_.store(0.0, std::memory_order_relaxed);
}

void JitterEstimator::observe(std::uint16_t seq, std::uint32_t timestamp, std::uint32_t ssrc,
    std::int64_t arrival_ns) noexcept
{
    // SSRC 变化 = 新流：旧时间轴作废，全重置后按首包处理（JB 侧 SSRC 钉住同模型）。
    if (have_packets_ && ssrc != current_ssrc_) {
        // 控制面日志（#3，见本文件顶部说明）：SSRC 变化 = 新流，旧时间轴作废。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
        log_debug_fmt(
            "JitterEstimator reset on SSRC change: 0x{:08X} -> 0x{:08X} (packets={} stalls={} jit_ms={:.2f} peak_ms={:.1f})",
            current_ssrc_, ssrc, packets_.load(std::memory_order_relaxed),
            stall_events_.load(std::memory_order_relaxed), jitter_ms_, stall_peak_ms_);
#endif
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
        // 控制面日志（#3）：开局窗口期的第一块拼图——锚点建立时刻。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
        log_debug_fmt(
            "JitterEstimator anchor established (first packet): seq={} ts={} ssrc=0x{:08X} arrival_ns={}",
            seq, timestamp, ssrc, arrival_ns);
#endif
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
        window_bits_ = (shift >= config::JB_ESTIMATOR_REORDER_WINDOW_PACKETS) ? 0 : (window_bits_ << shift);
        window_bits_ |= 1;
    } else {
        const std::uint64_t behind = max_ext_ - ext;
        if (behind < config::JB_ESTIMATOR_REORDER_WINDOW_PACKETS && (window_bits_ & (std::uint64_t { 1 } << behind)) != 0) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
        } else if (behind < config::JB_ESTIMATOR_REORDER_WINDOW_PACKETS) {
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
        // 时间轴重置 → transit 基准清零，差分基准与电平跟随同步归零，否则下一包的
        // transit（新锚点下接近 0）相对旧值会被误判成一次反向台阶。
        prev_transit_ms_ = 0.0;
        transit_level_ms_ = 0.0;
        transit_level_ms_out_.store(0.0, std::memory_order_relaxed);
        // 控制面日志（#3）：发送端时间轴重置 → transit 锚点重建，本包不进 J。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
        log_debug_fmt(
            "JitterEstimator transit anchor rebuilt (sender timeline reset): delta_ts={} seq={} interval_ms={:.2f} jit_ms={:.2f}",
            delta_ts, seq, arrival_interval_ms, jitter_ms_);
#endif
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
    // stall 峰值跟踪（NetEq DelayManager 的 peak detection 思路）：被 stall 门
    // 剔除出 J 的到达间隙是"buffer 必须独自挺过的最长时间"，近期最坏值直接
    // 指示 target 需要的水位。每包先按经过时间线性衰减（无新 stall 时缓慢
    // 忘记，拥塞结束后 target 才能回落），stall 时刷新为 max。衰减与刷新都
    // 只在 strand 内，对外经 stall_peak_ms_out_ 原子发布。
    if (stall_peak_last_ns_ == 0) {
        stall_peak_last_ns_ = arrival_ns; // 首个时间样本：只建衰减基线
    } else if (arrival_ns > stall_peak_last_ns_) {
        const double elapsed_ms
            = static_cast<double>(arrival_ns - stall_peak_last_ns_) / kNsPerMs;
        stall_peak_ms_ = std::max(0.0,
            stall_peak_ms_ - elapsed_ms * stall_peak_decay_ms_per_sec_ / 1000.0);
        stall_peak_last_ns_ = arrival_ns;
    }
    if (is_stall) {
        // 控制面日志（#2）：峰值刷新前→后必须可见，否则"峰值为什么挂这么久"
        // 只能靠倒推。阈值同时给出包周期倍数，便于对照 --jb-stall-threshold。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
        const double peak_before_ms = stall_peak_ms_;
#endif
        stall_events_.fetch_add(1, std::memory_order_relaxed);
        last_stall_gap_ms_.store(arrival_interval_ms, std::memory_order_relaxed);
        stall_peak_ms_ = std::max(stall_peak_ms_, arrival_interval_ms);
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
        log_debug_fmt(
            "JitterEstimator stall: gap={:.2f}ms threshold={:.2f}ms({:.2f} pkt) peak {:.2f} -> {:.2f}ms decay={:.1f}ms/s stalls={} seq={} jit_ms={:.2f}(kept)",
            arrival_interval_ms, stall_threshold_ms_,
            packet_ms_ > 0.0 ? stall_threshold_ms_ / packet_ms_ : 0.0,
            peak_before_ms, stall_peak_ms_, stall_peak_decay_ms_per_sec_,
            stall_events_.load(std::memory_order_relaxed), seq, jitter_ms_);
#endif
    } else {
        // RFC 3550 A.8：J += (|D| - J) / 16。stall 样本不入统计。
        const double abs_d = diff_ms >= 0.0 ? diff_ms : -diff_ms;
        jitter_ms_ += (abs_d - jitter_ms_) * kRfc3550Gain;
        jitter_ms_out_.store(jitter_ms_, std::memory_order_relaxed);
    }
    stall_peak_ms_out_.store(stall_peak_ms_, std::memory_order_relaxed);

    // 相对 transit：锚点差分，timestamp 随机 offset 消去。
    const double transit_ms = static_cast<double>(arrival_ns - anchor_arrival_ns_) / kNsPerMs
        - static_cast<double>(static_cast<std::int32_t>(timestamp - anchor_timestamp_)) * 1000.0
            / timestamp_rate_hz_;
    transit_ms_.store(transit_ms, std::memory_order_relaxed);
    // transit 台阶检测（纯诊断，不进控制）：相邻样本差分超阈值即记一次
    // （含 stall 包——stall 同时有 stall_events 行，两行对照即区分"抖动"与
    // "路径切换"）。注意必须对上一包差分：对慢电平比会在收敛期每包都超阈。
    // 电平用 EWMA 慢跟随：抖动只让它轻晃，路径电平变化让它整段搬家。
    {
        const double step = transit_ms - prev_transit_ms_;
        prev_transit_ms_ = transit_ms;
        if (step >= transit_step_threshold_ms_ || step <= -transit_step_threshold_ms_) {
            transit_step_events_.fetch_add(1, std::memory_order_relaxed);
            last_transit_step_ms_.store(step, std::memory_order_relaxed);
            // 控制面日志（见 doc/modules/observability.md 点位表）：transit 台阶的
            // 方向 + 幅度 + 电平前后值。stall 行回答"到达断了多久"，这行回答
            // "路径电平跳了多少"——同一事件的两面（大 stall 两行都有）。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
            log_debug_fmt(
                "JitterEstimator transit step: delta={:+.2f}ms level {:.2f} -> {:.2f}ms transit={:.2f}ms events={} seq={}",
                step, transit_level_ms_,
                transit_level_ms_ + (transit_ms - transit_level_ms_) * config::JB_TRANSIT_LEVEL_GAIN,
                transit_ms, transit_step_events_.load(std::memory_order_relaxed), seq);
#endif
        }
        transit_level_ms_
            += (transit_ms - transit_level_ms_) * config::JB_TRANSIT_LEVEL_GAIN;
        transit_level_ms_out_.store(transit_level_ms_, std::memory_order_relaxed);
    }
    if (!base_set_ || transit_ms < base_delay_ms_) {
        // 累积最小值 = 路径底噪。注意：这是 Phase 0 的简化口径（长期单调漂移
        // 下底噪只会偏低不会偏高；Phase 1 视数据换窗口最小值）。
        // 首个有效 transit 直接采用（base_set_ 标记，不拿 0.0 当哨兵——
        // 干净链路 transit 恒为 0，0.0 哨兵会让首个 spike 反噬 base）。
        base_delay_ms_ = transit_ms;
        base_set_ = true;
        base_delay_ms_out_.store(base_delay_ms_, std::memory_order_relaxed);
    }

    // 尾部直方图（默认策略的抖动项输入）：|到达间隔 - 发送间隔|（单包绝对偏差）。
    // 刻意不用 transit - base：累积最小 base 在持续漂移/阶跃下永不更新，
    // 相对值会永远钉在高位；差分天然漂移不变（漂移被 episodes + penalty 负责，
    // margin 只需覆盖网络抖动）。只进走到这里的包（按序 + 时间轴有效；
    // 乱序/迟到/重置包提前 return）。stall 包也进——它的 |D| 是合法尾部样本，
    // 只是密度不到 1% 时 P99 看不见（由 stall_peak 项负责它），分工明确。
    {
        double dev_ms = diff_ms >= 0.0 ? diff_ms : -diff_ms;
        if (!(dev_ms >= 0.0)) {
            dev_ms = 0.0; // NaN 防御，不应发生
        }
        // 先比较再转换：dev_ms 可能远大于 uint32 上限（病态 timestamp 差 /
        // 极低 timestamp_rate_hz），直接 static_cast 是 UB。
        std::uint32_t bucket = dev_ms >= static_cast<double>(config::JB_TAIL_HISTOGRAM_BUCKETS)
            ? config::JB_TAIL_HISTOGRAM_BUCKETS // ≥64ms 进溢出桶
            : static_cast<std::uint32_t>(dev_ms);
        if (tail_count_ < config::JB_TAIL_WINDOW_PACKETS) {
            ++tail_count_;
        } else {
            // 满窗：先退役最老样本，窗口滑出即遗忘（跌慢的来源）。
            const auto old = tail_ring_[tail_head_];
            if (old <= config::JB_TAIL_HISTOGRAM_BUCKETS) {
                --tail_hist_[old];
            }
        }
        tail_ring_[tail_head_] = bucket;
        ++tail_hist_[bucket];
        tail_head_ = (tail_head_ + 1) % config::JB_TAIL_WINDOW_PACKETS;
        // 分位点现算：64 桶线性扫找 ceil(q×n) 累积点（每包 O(64)，可忽略）。
        // q = config::JB_TAIL_QUANTILE（默认 0.99）。注意改 q 会让诊断列名 p99 不再贴切，
        // 所以要连字段名/文档一起改，别只动常量。
        const std::uint64_t threshold = static_cast<std::uint64_t>(
            std::ceil(static_cast<double>(tail_count_) * config::JB_TAIL_QUANTILE));
        std::uint64_t cum = 0;
        std::uint32_t p99 = 0;
        for (std::uint32_t b = 0; b <= config::JB_TAIL_HISTOGRAM_BUCKETS; ++b) {
            cum += tail_hist_[b];
            if (cum >= threshold) {
                p99 = b;
                break;
            }
        }
        tail_p99_ms_out_.store(static_cast<double>(p99), std::memory_order_relaxed);
        tail_samples_out_.store(tail_count_, std::memory_order_relaxed);
    }
    // 冷启动门：样本不足时 P99≈最大值（tail_count_ < JB_TAIL_MIN_SAMPLES(128) 时单个 spike 就是 P99），直接驱动
    // 会把启动期一次断流当成常态顶到顶。未满 JB_TAIL_MIN_SAMPLES 前发布 -1，
    // controller 按 tail<0 取抖动项 0 落地板（floor start）。诊断 p99 列 -1.0
    // 即此状态，tsamp 列看填充进度。
    if (tail_count_ < config::JB_TAIL_MIN_SAMPLES) {
        tail_p99_ms_out_.store(-1.0, std::memory_order_relaxed);
    }

    // 控制面日志（#3，见本文件顶部说明）：开局 J 从 0 到收敛的里程碑。
    // 只在 1/16/64/256 个有效样本上各打一行——开局 ~60ms 窗口期的 target
    // 行为此前完全黑盒，这四行把"J 收敛到稳态"的过程钉在时间轴上。
#if AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG
    const auto sample_index = ++jitter_sample_count_;
    if (sample_index == 1 || sample_index == 16 || sample_index == 64
        || sample_index == 256) {
        log_debug_fmt(
            "JitterEstimator J milestone: samples={} jit_ms={:.2f} transit_ms={:.1f} base_ms={:.1f} interval_ms={:.2f} peak_ms={:.1f}",
            sample_index, jitter_ms_, transit_ms, base_delay_ms_,
            arrival_interval_ms, stall_peak_ms_);
    }
#endif
}

JitterEstimates JitterEstimator::estimates() const noexcept
{
    JitterEstimates out;
    out.transit_ms = transit_ms_.load(std::memory_order_relaxed);
    out.transit_level_ms = transit_level_ms_out_.load(std::memory_order_relaxed);
    out.transit_step_events = transit_step_events_.load(std::memory_order_relaxed);
    out.last_transit_step_ms = last_transit_step_ms_.load(std::memory_order_relaxed);
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
    out.stall_peak_ms = stall_peak_ms_out_.load(std::memory_order_relaxed);
    out.tail_p99_ms = tail_p99_ms_out_.load(std::memory_order_relaxed);
    out.tail_samples = tail_samples_out_.load(std::memory_order_relaxed);
    return out;
}

} // namespace aqua::audio
