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
    std::size_t target_latency_packets,
    std::size_t capacity_packets,
    std::uint32_t drift_window_size,
    std::uint32_t drift_late_threshold)
    : format_(format)
    , target_latency_packets_(target_latency_packets)
    , capacity_(capacity_packets)
    , slot_mask_(capacity_packets - 1)
    , drift_window_size_(drift_window_size)
    , drift_late_threshold_(drift_late_threshold)
{
    // capacity 必须是 2 的幂
    if (capacity_packets == 0 || (capacity_packets & (capacity_packets - 1)) != 0) {
        throw std::invalid_argument("JitterBuffer capacity must be a power of two");
    }

    // target_latency_packets 必须 >= 1，否则首包 deadline = now，零缓冲。
    if (target_latency_packets == 0) {
        throw std::invalid_argument("JitterBuffer target_latency_packets must be >= 1");
    }

    // drift_window_size 必须 > 0，否则每个包都评估 late 比例。
    if (drift_window_size == 0) {
        throw std::invalid_argument("JitterBuffer drift_window_size must be > 0");
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

    // 计算每包时长（微秒）
    // packet_duration = frames_per_packet / sample_rate * 1,000,000 us
    packet_duration_ = std::chrono::microseconds(
        static_cast<std::int64_t>(frames_per_packet) * 1'000'000 / format_.sample_rate);

    // 预分配所有内存
    slots_.resize(capacity_);
    storage_.resize(capacity_ * payload_size_, std::byte { 0 });
}

void JitterBuffer::init_timeline(std::uint32_t sequence,
    std::span<const std::byte> payload)
{
    // 调用方（push）应已校验 payload 大小，此处 assert 防御未来新增调用路径遗漏。
    assert(payload.size() >= payload_size_);

    initialized_ = true;
    next_pop_seq_ = sequence;
    highest_pushed_seq_ = sequence;
    first_packet_time_ = clock::now();
    next_deadline_ = first_packet_time_ + packet_duration_ * target_latency_packets_;
    consecutive_late_ = 0;
    drift_late_count_ = 0;
    drift_total_count_ = 0;

    auto idx = sequence & slot_mask_;
    slots_[idx].sequence = sequence;
    slots_[idx].valid = true;
    std::memcpy(slot_payload(idx).data(), payload.data(), payload_size_);
}

void JitterBuffer::push(std::uint32_t sequence,
    std::uint32_t /*sample_position*/,
    std::span<const std::byte> payload)
{
    if (payload.size() != payload_size_) {
        aqua::log_debug_fmt("JitterBuffer push: payload size mismatch ({} != {})",
            payload.size(), payload_size_);
        return;
    }

    packets_received_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(slots_mutex_);

    // 第一个包：初始化播放时间线，不参与 drift 检测
    if (!initialized_) {
        init_timeline(sequence, payload);
        return;
    }

    // 漂移检测：窗口满时检查 late 比例
    if (drift_total_count_ >= drift_window_size_) {
        if (drift_late_count_ >= drift_late_threshold_) {
            aqua::log_warn_fmt(
                "JitterBuffer: clock drift detected ({} late/{} packets = {:.1f}%), rebasing timeline to seq={}",
                drift_late_count_, drift_total_count_,
                static_cast<double>(drift_late_count_) * 100.0 / drift_total_count_,
                sequence);
            init_timeline(sequence, payload);
            return;
        }
        drift_late_count_ = 0;
        drift_total_count_ = 0;
    }
    ++drift_total_count_;

    // 与 next_pop_seq_ 的有符号差值
    auto diff = seq_diff(sequence, next_pop_seq_);

    if (diff < 0) {
        // 已经过了这个 sequence 的播放时刻（或重复包）
        auto idx = sequence & slot_mask_;
        if (slots_[idx].valid && slots_[idx].sequence == sequence) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            // 重复包不影响连续 late 计数（不算新到达的 late 包）
        } else {
            late_packets_.fetch_add(1, std::memory_order_relaxed);
            ++drift_late_count_;
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
            }
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

    slots_[idx].sequence = sequence;
    slots_[idx].valid = true;
    std::memcpy(slot_payload(idx).data(), payload.data(), payload_size_);

    // 更新最高已 push 的 sequence
    if (seq_diff(sequence, highest_pushed_seq_) > 0) {
        highest_pushed_seq_ = sequence;
    }
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

    // 尝试输出 next_pop_seq_ 的数据
    auto idx = next_pop_seq_ & slot_mask_;
    bool got_real_data = false;

    if (slots_[idx].valid && slots_[idx].sequence == next_pop_seq_) {
        // 包存在：输出真实 PCM
        std::memcpy(output.data(), slot_payload(idx).data(), payload_size_);
        slots_[idx].valid = false;
        got_real_data = true;
    } else {
        // 包不存在（丢包或还没到）：静音填充
        std::memset(output.data(), 0, payload_size_);
        packets_lost_.fetch_add(1, std::memory_order_relaxed);
    }

    // 推进到下一个 sequence
    ++next_pop_seq_;

    // 计算下一个 deadline：基于上一个 deadline + packet_duration
    next_deadline_ = next_deadline_ + packet_duration_;

    return got_real_data;
}

void JitterBuffer::reset()
{
    std::lock_guard<std::mutex> lock(slots_mutex_);

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
    drift_late_count_ = 0;
    drift_total_count_ = 0;

    // 不清除统计计数器（packets_received_ / packets_lost_ / duplicates_ / late_packets_）
    // 统计在 session 生命周期内累积，reset 只重置播放状态
}

std::uint64_t JitterBuffer::packets_received() const noexcept { return packets_received_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::packets_lost() const noexcept { return packets_lost_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::duplicates() const noexcept { return duplicates_.load(std::memory_order_relaxed); }

std::uint64_t JitterBuffer::late_packets() const noexcept { return late_packets_.load(std::memory_order_relaxed); }

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
