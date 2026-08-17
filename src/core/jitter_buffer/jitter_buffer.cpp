#include "core/jitter_buffer/jitter_buffer.h"
#include "core/logger/logger.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace aqua::jitter {

JitterBuffer::JitterBuffer(const AudioFormat& format,
    std::uint32_t frames_per_packet,
    std::size_t floor_packets,
    std::size_t capacity_packets,
    std::uint32_t detect_window_packets,
    std::uint32_t drift_rebase_late_count,
    std::optional<AdaptiveTargetConfig> adaptive)
    : format_(format)
    , target_latency_packets_(floor_packets)
    , floor_packets_(floor_packets)
    , adaptive_(adaptive.has_value())
    , adapt_cfg_(adaptive.value_or(AdaptiveTargetConfig {}))
    , capacity_(capacity_packets)
    , slot_mask_(capacity_packets - 1)
    , detect_window_packets_(detect_window_packets)
    , drift_rebase_late_count_(drift_rebase_late_count)
{
    // capacity 必须是 2 的幂
    if (capacity_packets == 0 || (capacity_packets & (capacity_packets - 1)) != 0) {
        throw std::invalid_argument("JitterBuffer capacity must be a power of two");
    }

    // floor 必须 >= 1，否则首包 deadline = now，零缓冲。
    if (floor_packets == 0) {
        throw std::invalid_argument("JitterBuffer floor_packets must be >= 1");
    }

    // capacity 必须 >= floor * 2（给乱序留余量，见 §22.9 契约）。
    // 用除法形式避免 floor*2 的 size_t 溢出。
    if (floor_packets > capacity_packets / 2) {
        throw std::invalid_argument("JitterBuffer capacity must be >= floor_packets * 2");
    }

    // 自适应上限（显式指定时）：必须落在 [floor, capacity/2]。
    // 上界维持乱序余量契约；下界保证 ceiling 不低于 floor（否则自适应永远无法抬升）。
    if (adaptive && adapt_cfg_.max_packets) {
        if (*adapt_cfg_.max_packets < floor_packets
            || *adapt_cfg_.max_packets > capacity_packets / 2) {
            throw std::invalid_argument(
                "JitterBuffer adaptive max_packets must be in [floor_packets, capacity/2]");
        }
    }

    // 检测窗口必须 > 0，否则每个包都评估一次。
    if (detect_window_packets == 0) {
        throw std::invalid_argument("JitterBuffer detect_window_packets must be > 0");
    }

    // drift rebase 阈值必须 > 0，否则每个窗口都触发时间线重建。
    if (drift_rebase_late_count == 0) {
        throw std::invalid_argument("JitterBuffer drift_rebase_late_count must be > 0");
    }

    // format 必须合法、frames_per_packet 必须 > 0，否则 payload_size_ 为 0
    // （push 全部被 payload 长度校验丢弃）且 packet_duration_ 为 0（调度 deadline
    // 恒等于首包时间，外部调度器空转）。调用方当前保证合法，此处防御未来误用。
    if (!format_.valid()) {
        throw std::invalid_argument("JitterBuffer requires a valid AudioFormat");
    }
    if (frames_per_packet == 0) {
        throw std::invalid_argument("JitterBuffer frames_per_packet must be > 0");
    }

    // 计算每包 PCM 字节数
    payload_size_ = static_cast<std::size_t>(frames_per_packet) * format_.frame_bytes();

    // 计算每包时长（纳秒）。
    // 用纳秒而非微秒：微秒整数除法在 44.1kHz 家族（含因子 7，如 44100/88200）下被截断
    // —— 144/44100s = 3265.306us → 3265us，每包漂移 0.306us（~93.75ppm）。
    // next_deadline_ 每次 pop 累加一次 packet_duration_，漂移随之累积，约 10 分钟
    // 排空 JB 缓冲触发 rebase。纳秒精度下漂移降至 ~0.037ppm，可忽略。
    packet_duration_ = std::chrono::nanoseconds(
        static_cast<std::int64_t>(frames_per_packet) * 1'000'000'000 / format_.sample_rate);

    // 预分配所有内存
    slots_.resize(capacity_);
    storage_.resize(capacity_ * payload_size_, std::byte { 0 });
    last_pcm_.resize(payload_size_, std::byte { 0 });
}

void JitterBuffer::init_timeline(std::uint32_t sequence,
    std::span<const std::byte> payload)
{
    // 调用方（push）应已校验 payload 大小，此处 assert 防御未来新增调用路径遗漏。
    assert(payload.size() >= payload_size_);

    const bool rebase = initialized_;

    // 软 rebase 时 sequence 可能向回跳（consecutive-late 场景），slot 中可能
    // 仍留有更高 seq 的 future 包：保留 highest_pushed_seq_ 的最大值，
    // 避免 buffer_fill_packets() 的统计范围瞬时低估。
    if (!rebase || seq_diff(sequence, highest_pushed_seq_) > 0) {
        highest_pushed_seq_ = sequence;
    }

    initialized_ = true;
    first_packet_time_ = clock::now();

    // deadline 策略分场景：
    // - 首包：now + target×duration（标准起播缓冲）。
    // - rebase 保持节奏（核心改进）：旧策略统一 now + target×duration 意味着每次
    //   rebase 都停供 target 毫秒，WASAPI 侧持续消费会把 RB 打穿造成可闻静音。
    //   新策略按缺口大小分档，维持供给连续性：
    //   * 小前跳（0 < diff <= target）：deadline 沿原 cadence 推进 diff 拍，
    //     缺口包由 pop_next 的 PLC 填充，供给不停顿。
    //   * 大断裂（diff > target）：缺口远超缓冲预算，PLC 无意义，重新缓冲
    //     （语义与首包一致，停顿是必要的）。
    //   * 回跳（diff < 0，暂停/恢复场景）：pop 空转期 deadline ≈ now 连续推进，
    //     rebase 包从下一拍开始播放，不停顿。
    //   * diff == 0：sequence 即原 next_pop_seq_，deadline 本就属于它，不动。
    if (!rebase) {
        next_deadline_ = first_packet_time_
            + packet_duration_ * static_cast<std::int64_t>(target_latency_packets_);
    } else {
        const auto diff = seq_diff(sequence, next_pop_seq_);
        const auto diff_i64 = static_cast<std::int64_t>(diff);
        if (diff > 0 && static_cast<std::size_t>(diff) <= target_latency_packets_) {
            next_deadline_ = next_deadline_
                + packet_duration_ * diff_i64;
        } else if (diff > 0) {
            next_deadline_ = first_packet_time_
                + packet_duration_ * static_cast<std::int64_t>(target_latency_packets_);
        } else if (diff < 0) {
            next_deadline_ = next_deadline_ + packet_duration_;
        }
        // diff == 0: deadline 不变
    }

    next_pop_seq_ = sequence;
    consecutive_late_ = 0;
    window_late_count_ = 0;
    window_total_count_ = 0;
    adapt_clean_streak_ = 0; // rebase = 不稳定事件，重置回落的连续干净资格（保留已学习的 target）
    if (rebase) {
        rebases_.fetch_add(1, std::memory_order_relaxed);
    }

    auto idx = sequence & slot_mask_;
    slots_[idx].sequence = sequence;
    slots_[idx].valid = true;
    std::memcpy(slot_payload(idx).data(), payload.data(), payload_size_);
}

void JitterBuffer::push(std::uint32_t sequence,
    std::span<const std::byte> payload)
{
    if (payload.size() != payload_size_) {
        aqua::log_debug_fmt("JitterBuffer push: payload size mismatch ({} != {})",
            payload.size(), payload_size_);
        malformed_packets_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    packets_received_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(slots_mutex_);

    // 第一个包：初始化播放时间线，不参与检测窗口
    if (!initialized_) {
        init_timeline(sequence, payload);
        return;
    }

    // 与 next_pop_seq_ 的有符号差值
    auto diff = seq_diff(sequence, next_pop_seq_);

    if (diff < 0) {
        // 已经过了这个 sequence 的播放时刻（或重复包）
        auto idx = sequence & slot_mask_;
        if (slots_[idx].valid && slots_[idx].sequence == sequence) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            // 重复包不计入检测窗口，避免稀释 late 比例导致 rebase 变钝
        } else {
            late_packets_.fetch_add(1, std::memory_order_relaxed);
            ++window_late_count_;
            ++window_total_count_;
            // 连续 late 检测：音频源暂停后恢复时，pop 空转已让 next_pop_seq_ 超前，
            // 新包全部 diff<0，无法触发 diff>=capacity 的 reset，导致永久死锁。
            // 连续 late 达到 capacity 时强制 reset 重建时间线。
            if (++consecutive_late_ >= capacity_) {
                aqua::log_warn_fmt("JitterBuffer: {} consecutive late packets (seq={}, next_pop={}, diff={}), "
                                   "timeline desync detected (likely audio source pause/resume), rebasing timeline",
                    consecutive_late_, sequence, next_pop_seq_, diff);
                // 软 rebase：不调用 reset()，保留 slot 中已有的 future 包。
                // init_timeline 重置 next_pop_seq_ 和 deadline，stale slot 会被
                // pop_next 的 sequence 校验自然过滤。
                init_timeline(sequence, payload);
                return;
            }
            // 迟到包：即便触发 drift rebase（init_timeline 已存储本包），本分支也到此返回，
            // 返回值无需处理，显式忽略以表达该意图。
            (void)evaluate_detect_window_locked(sequence, payload);
        }
        return;
    }

    if (static_cast<std::size_t>(diff) >= capacity_) {
        // 跳跃太远（diff >= capacity），可能是严重乱序或调度延迟积累。
        // 软 rebase：不调用 reset()，保留 slot 中已缓冲的 future 包。
        // init_timeline 将 next_pop_seq_ 跳到当前包，重建 deadline。
        // 跳过区间内的包计为 lost（pop_next 时静音填充），但已缓冲的
        // future 包（seq >= 新 next_pop_seq_）不受影响，继续正常播放。
        aqua::log_warn_fmt("JitterBuffer: sequence jump too far (seq={}, next_pop={}, diff={}), rebasing timeline",
            sequence, next_pop_seq_, diff);
        init_timeline(sequence, payload);
        return;
    }

    // diff >= 0 and diff < capacity：expected / future 包，复位连续 late 计数
    consecutive_late_ = 0;

    auto idx = sequence & slot_mask_;
    if (slots_[idx].valid && slots_[idx].sequence == sequence) {
        // 重复包
        duplicates_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++window_total_count_;
    if (evaluate_detect_window_locked(sequence, payload)) {
        return; // drift rebase 已接管本包（init_timeline 已存储）
    }

    slots_[idx].sequence = sequence;
    slots_[idx].valid = true;
    std::memcpy(slot_payload(idx).data(), payload.data(), payload_size_);

    // 更新最高已 push 的 sequence
    if (seq_diff(sequence, highest_pushed_seq_) > 0) {
        highest_pushed_seq_ = sequence;
    }
}

bool JitterBuffer::evaluate_detect_window_locked(std::uint32_t sequence,
    std::span<const std::byte> payload)
{
    if (window_total_count_ < detect_window_packets_) {
        return false;
    }

    // 窗口满：取出计数并重置。drift rebase 与 AIMD 依次用同一份计数评估。
    const auto window_late = window_late_count_;
    const auto window_total = window_total_count_;
    window_late_count_ = 0;
    window_total_count_ = 0;

    // 1) drift rebase：late 比例超阈值 → 时间线失步（两端时钟速率差），重建。
    if (window_late >= drift_rebase_late_count_) {
        aqua::log_warn_fmt(
            "JitterBuffer: clock drift detected ({} late/{} packets = {:.1f}%), rebasing timeline to seq={}",
            window_late, window_total,
            static_cast<double>(window_late) * 100.0 / window_total,
            sequence);
        init_timeline(sequence, payload); // 内部重置窗口计数与干净资格
        return true;
    }

    // 2) AIMD（未启用自适应时到此为止）
    if (!adaptive_) {
        return false;
    }

    const auto target_ms = [this] {
        return std::chrono::duration<double, std::milli>(
                   packet_duration_ * static_cast<std::int64_t>(target_latency_packets_))
            .count();
    };

    if (window_late >= adapt_cfg_.raise_late_count) {
        // 快升：late 压力 → target +1 包，deadline 后移 1 拍（下游 RB 蓄水 1 拍的量）。
        // 上限：显式 max_packets 优先，否则 capacity/2（乱序余量契约的默认上界）。
        const std::size_t adapt_ceiling = adapt_cfg_.max_packets.value_or(capacity_ / 2);
        adapt_clean_streak_ = 0;
        if (target_latency_packets_ < adapt_ceiling) {
            ++target_latency_packets_;
            next_deadline_ += packet_duration_;
            log_info_fmt("JitterBuffer: adaptive target raised to {} packets ({:.1f}ms, "
                         "ceiling {} packets), {} late in last {} packets",
                target_latency_packets_, target_ms(), adapt_ceiling,
                window_late, detect_window_packets_);
        }
    } else if (window_late == 0) {
        // 慢降：连续干净窗口 → target -1 包，deadline 前移 1 拍（排水）。
        // 钳到 now：deadline 已落后（追赶/RB 满排水中）时前移会放大滞后，
        // 极端情况触发 pop_next 的断流 reset；钳位后立即恢复 cadence，同样净减 1 拍。
        if (++adapt_clean_streak_ >= adapt_cfg_.lower_clean_windows
            && target_latency_packets_ > floor_packets_) {
            --target_latency_packets_;
            const auto now = clock::now();
            next_deadline_ = (next_deadline_ - packet_duration_ < now)
                ? now
                : next_deadline_ - packet_duration_;
            adapt_clean_streak_ = 0;
            log_info_fmt("JitterBuffer: adaptive target lowered to {} packets ({:.1f}ms), "
                         "floor {} packets, {} consecutive clean windows",
                target_latency_packets_, target_ms(), floor_packets_,
                adapt_cfg_.lower_clean_windows);
        }
    } else {
        // 迟滞带（0 < late < raise 阈值）：保持，打断连续干净计数
        adapt_clean_streak_ = 0;
    }
    return false;
}

std::optional<JitterBuffer::time_point>
JitterBuffer::next_playout_deadline() const noexcept
{
    if (!initialized_) {
        return std::nullopt;
    }
    return next_deadline_;
}

bool JitterBuffer::pop_next(std::span<std::byte> output)
{
    if (output.size() < payload_size_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(slots_mutex_);

    // 防御：首个包到达前没有时间线，next_pop_seq_ / next_deadline_ 均为初值，
    // 此时 pop 会静音填充并推进一个无意义的时间线。外部调度器本应在
    // next_playout_deadline() 返回 nullopt 时跳过 pop，此处兜底。
    if (!initialized_) {
        return false;
    }

    // 长时间断流检测：deadline 落后超过 max(target 缓冲量, MIN_RESET_LATENESS) 时，
    // 逐包追赶无意义（会输出大量迟到数据 + deadline_miss 风暴），重置时间线等待
    // 下一个包重建。阈值取两者较大值：小 target（< 16ms）时调度器定时器批量唤醒
    // （Windows 粒度 ~15.6ms）的合法落后会被纯 target 阈值误判为断流（reset 风暴）。
    // 注意：乘法需转 int64——size_t 与 chrono rep(int64) 混合会把 common rep 变成
    // 无符号类型，负的 lateness（deadline 在未来）被回绕成巨大正数导致误触发。
    const auto now = clock::now();
    const auto max_lateness = std::max<std::chrono::nanoseconds>(
        packet_duration_ * static_cast<std::int64_t>(target_latency_packets_),
        config::JITTER_MIN_RESET_LATENESS_MS);
    if (now - next_deadline_ > max_lateness) {
        aqua::log_warn_fmt("JitterBuffer: playout deadline behind by {}ms (likely stream gap), resetting timeline",
            std::chrono::duration_cast<std::chrono::milliseconds>(now - next_deadline_).count());
        reset_playout_state_locked();
        std::memset(output.data(), 0, payload_size_);
        return false;
    }

    // 尝试输出 next_pop_seq_ 的数据
    auto idx = next_pop_seq_ & slot_mask_;
    bool got_real_data = false;

    if (slots_[idx].valid && slots_[idx].sequence == next_pop_seq_) {
        // 包存在：输出真实 PCM，并刷新 PLC 历史
        std::memcpy(output.data(), slot_payload(idx).data(), payload_size_);
        std::memcpy(last_pcm_.data(), output.data(), payload_size_);
        hide_gain_ = 1.0f;
        slots_[idx].valid = false;
        got_real_data = true;
    } else {
        // 包不存在（丢包或还没到）：丢包隐藏（PLC）——重复上一包 PCM 并逐包衰减，
        // 比纯静音的"咔哒"声更平滑。连续丢包每包增益减半，若干包后收敛为静音。
        packets_lost_.fetch_add(1, std::memory_order_relaxed);
        if (hide_gain_ > 0.0f) {
            hide_gain_ *= 0.5f;
            std::memcpy(output.data(), last_pcm_.data(), payload_size_);
            apply_gain({ output.data(), payload_size_ }, hide_gain_);
        } else {
            std::memset(output.data(), 0, payload_size_);
        }
    }

    // 推进到下一个 sequence
    ++next_pop_seq_;

    // 计算下一个 deadline：基于上一个 deadline + packet_duration
    next_deadline_ = next_deadline_ + packet_duration_;

    return got_real_data;
}

void JitterBuffer::apply_gain(std::span<std::byte> pcm, float gain) const noexcept
{
    // 逐样本 memcpy 读写，不依赖缓冲区对齐。样本数 = pcm / bytes_per_sample。
    const std::size_t sample_bytes = format_.bytes_per_sample();
    if (sample_bytes == 0) {
        std::memset(pcm.data(), 0, pcm.size());
        return;
    }
    const std::size_t samples = pcm.size() / sample_bytes;

    switch (format_.encoding) {
    case AudioEncoding::PcmS16LE: {
        for (std::size_t i = 0; i < samples; ++i) {
            std::int16_t v;
            std::memcpy(&v, pcm.data() + i * 2, sizeof(v));
            v = static_cast<std::int16_t>(static_cast<float>(v) * gain);
            std::memcpy(pcm.data() + i * 2, &v, sizeof(v));
        }
        break;
    }
    case AudioEncoding::PcmS32LE: {
        for (std::size_t i = 0; i < samples; ++i) {
            std::int32_t v;
            std::memcpy(&v, pcm.data() + i * 4, sizeof(v));
            v = static_cast<std::int32_t>(static_cast<float>(v) * gain);
            std::memcpy(pcm.data() + i * 4, &v, sizeof(v));
        }
        break;
    }
    case AudioEncoding::PcmF32LE: {
        for (std::size_t i = 0; i < samples; ++i) {
            float v;
            std::memcpy(&v, pcm.data() + i * 4, sizeof(v));
            v *= gain;
            std::memcpy(pcm.data() + i * 4, &v, sizeof(v));
        }
        break;
    }
    default:
        // S24LE/U8 打包格式解包成本高且极少使用：回退静音。
        std::memset(pcm.data(), 0, pcm.size());
        break;
    }
}

void JitterBuffer::reset()
{
    std::lock_guard<std::mutex> lock(slots_mutex_);
    reset_playout_state_locked();
}

void JitterBuffer::reset_playout_state_locked()
{
    // 只清除 slot metadata，不清 storage_（旧数据不会被读取因为 valid=false）
    for (auto& slot : slots_) {
        slot.valid = false;
    }

    initialized_ = false;
    next_pop_seq_ = 0;
    highest_pushed_seq_ = 0;
    next_deadline_ = { };
    first_packet_time_ = { };
    consecutive_late_ = 0;
    window_late_count_ = 0;
    window_total_count_ = 0;
    // 自适应：窗口统计随时间线作废（断流期间的 late/干净不再代表当前网络），
    // 但保留已学习的 target——网络状况跨断流持续。
    adapt_clean_streak_ = 0;
    hide_gain_ = 0.0f; // PLC 失效（时间线已重置，上一包历史无意义），直到下一个真实包

    // 不清除统计计数器（packets_received_ / packets_lost_ / duplicates_ / late_packets_ / malformed_packets_）
    // 统计在 session 生命周期内累积，reset 只重置播放状态
}

std::uint64_t JitterBuffer::packets_received() const noexcept { return packets_received_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::packets_lost() const noexcept { return packets_lost_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::duplicates() const noexcept { return duplicates_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::late_packets() const noexcept { return late_packets_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::malformed_packets() const noexcept { return malformed_packets_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::rebases() const noexcept { return rebases_.load(std::memory_order_relaxed); }

std::size_t JitterBuffer::target_latency_packets() const noexcept
{
    std::lock_guard<std::mutex> lock(slots_mutex_);
    return target_latency_packets_;
}

std::size_t JitterBuffer::buffer_fill_packets() const noexcept
{
    std::lock_guard<std::mutex> lock(slots_mutex_);

    if (!initialized_) {
        return 0;
    }

    // 计算已 push 但尚未 pop 的有效包数
    std::size_t count = 0;
    auto diff = seq_diff(highest_pushed_seq_, next_pop_seq_);
    if (diff < 0) {
        return 0;
    }

    // 遍历 [next_pop_seq_, highest_pushed_seq_] 范围内的 slot
    for (int32_t i = 0; i <= diff && static_cast<std::size_t>(i) < capacity_; ++i) {
        auto seq = next_pop_seq_ + static_cast<std::uint32_t>(i);
        auto idx = seq & slot_mask_;
        if (slots_[idx].valid && slots_[idx].sequence == seq) {
            ++count;
        }
    }
    return count;
}

std::size_t JitterBuffer::capacity_packets() const noexcept { return capacity_; }

std::uint32_t JitterBuffer::next_sequence() const noexcept
{
    std::lock_guard<std::mutex> lock(slots_mutex_);
    return next_pop_seq_;
}

} // namespace aqua::jitter
