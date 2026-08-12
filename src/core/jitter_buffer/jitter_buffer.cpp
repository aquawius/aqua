#include "core/jitter_buffer/jitter_buffer.h"
#include "core/logger/logger.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace aqua::jitter {

JitterBuffer::JitterBuffer(const AudioFormat& format,
                           std::uint32_t frames_per_packet,
                           std::size_t target_latency_packets,
                           std::size_t capacity_packets)
    : format_(format)
    , target_latency_packets_(target_latency_packets)
    , capacity_(capacity_packets)
    , slot_mask_(capacity_packets - 1)
{
    // capacity 必须是 2 的幂
    if (capacity_packets == 0 ||
        (capacity_packets & (capacity_packets - 1)) != 0) {
        throw std::invalid_argument("JitterBuffer capacity must be a power of two");
    }

    // 计算每包 PCM 字节数
    payload_size_ = static_cast<std::size_t>(frames_per_packet) * format_.frame_bytes();

    // 计算每包时长（微秒）
    // packet_duration = frames_per_packet / sample_rate * 1,000,000 us
    packet_duration_ = std::chrono::microseconds(
        static_cast<std::int64_t>(frames_per_packet) * 1'000'000 / format_.sample_rate);

    // 预分配所有内存
    slots_.resize(capacity_);
    storage_.resize(capacity_ * payload_size_, std::byte{0});
}

void JitterBuffer::init_timeline(std::uint32_t sequence,
                                 std::span<const std::byte> payload)
{
    initialized_ = true;
    next_pop_seq_ = sequence;
    highest_pushed_seq_ = sequence;
    first_packet_time_ = clock::now();
    next_deadline_ = first_packet_time_ + packet_duration_ * target_latency_packets_;

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

    ++packets_received_;

    // 第一个包：初始化播放时间线
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
            ++duplicates_;
        } else {
            ++late_packets_;
        }
        return;
    }

    if (static_cast<std::size_t>(diff) >= capacity_) {
        // 跳跃太远（diff >= capacity），可能是严重乱序或 session 重置
        aqua::log_warn_fmt("JitterBuffer: sequence jump too far (seq={}, next_pop={}, diff={}), resetting",
                           sequence, next_pop_seq_, diff);
        reset();
        init_timeline(sequence, payload);
        return;
    }

    // diff >= 0 and diff < capacity：expected / future 包
    auto idx = sequence & slot_mask_;
    if (slots_[idx].valid && slots_[idx].sequence == sequence) {
        // 重复包
        ++duplicates_;
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
        ++packets_lost_;
    }

    // 推进到下一个 sequence
    ++next_pop_seq_;

    // 计算下一个 deadline：基于上一个 deadline + packet_duration
    next_deadline_ = next_deadline_ + packet_duration_;

    return got_real_data;
}

void JitterBuffer::reset()
{
    // 只清除 slot metadata，不清 storage_（旧数据不会被读取因为 valid=false）
    for (auto& slot : slots_) {
        slot.valid = false;
    }

    initialized_ = false;
    next_pop_seq_ = 0;
    highest_pushed_seq_ = 0;
    next_deadline_ = {};
    first_packet_time_ = {};

    // 不清除统计计数器（packets_received_ / packets_lost_ / duplicates_ / late_packets_）
    // 统计在 session 生命周期内累积，reset 只重置播放状态
}

std::size_t JitterBuffer::buffer_fill_packets() const noexcept
{
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

} // namespace aqua::jitter
