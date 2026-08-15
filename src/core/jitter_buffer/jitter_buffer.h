#ifndef AQUA_JITTER_BUFFER_H
#define AQUA_JITTER_BUFFER_H

#include "core/public/audio_format.h"
#include "core/public/config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace aqua::jitter {

// JitterBuffer：在固定播放延迟下处理 UDP 乱序、重复、丢包和 late packet。
//
// 核心设计：
// - push 时不判定丢包，只归类（expected / future / duplicate / late）
// - 只有超过 playout deadline 才判定 lost 并静音填充
// - 预分配连续 PCM storage，热路径零 heap allocation
// - 不依赖 timer，只暴露 next_playout_deadline() 供外部调度器使用
//
// Threading contract:
//   push() 和 pop_next() 必须在同一个 executor / 线程中调用。
//   当前设计为 io_context 单线程，push 来自 UDP 回调，pop_next 来自 steady_timer 回调。
//   slots_ 访问由 slots_mutex_ 保护，允许诊断 getter（buffer_fill_packets）从其他线程安全读取。
//   统计计数器（packets_received_ 等）为 atomic，可从任意线程读取。
class JitterBuffer {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // 构造时预分配所有内存。
    // format:           音频格式（决定 frame_bytes）
    // frames_per_packet: 每包帧数（决定 payload_size 和 packet_duration）
    // target_latency_packets: 初始缓冲包数（如 48kHz 下 10 包 = 30ms）
    // capacity_packets: ring 容量，必须为 2 的幂，>= target_latency_packets * 2
    // drift_window_size: 漂移检测滑动窗口大小（包数），默认 config.h 值
    // drift_late_threshold: 窗口内 late 包数 >= 此值时触发 rebase，默认 config.h 值
    JitterBuffer(const AudioFormat& format,
        std::uint32_t frames_per_packet,
        std::size_t target_latency_packets,
        std::size_t capacity_packets,
        std::uint32_t drift_window_size = aqua::config::JITTER_DRIFT_WINDOW_SIZE,
        std::uint32_t drift_late_threshold = aqua::config::JITTER_DRIFT_LATE_THRESHOLD);

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    // UDP I/O 线程调用：推入收到的音频包。
    // 自动归类：expected / future / duplicate / late。
    // push 时不判定丢包。
    void push(std::uint32_t sequence,
        std::uint32_t sample_position,
        std::span<const std::byte> payload);

    // 外部调度器查询下一次播放 deadline。
    // 返回 nullopt 表示尚未收到第一个包。
    [[nodiscard]] std::optional<time_point> next_playout_deadline() const noexcept;

    // deadline 到达后调用。输出 payload_size 字节：真实 PCM 或静音填充。
    // 返回 true 表示输出了真实 PCM，false 表示输出了静音（丢包）。
    // output 的大小必须 >= payload_size。
    [[nodiscard]] bool pop_next(std::span<std::byte> output);

    // 重置播放状态（如严重乱序或 session 重置时）。
    // 只清除 slot 和 timeline 状态，不清除统计计数器。
    void reset();

    // ---- Diagnostics ----

    [[nodiscard]] std::uint64_t packets_received() const noexcept;
    [[nodiscard]] std::uint64_t packets_lost() const noexcept;
    [[nodiscard]] std::uint64_t duplicates() const noexcept;
    [[nodiscard]] std::uint64_t late_packets() const noexcept;
    [[nodiscard]] std::size_t buffer_fill_packets() const noexcept;
    [[nodiscard]] std::size_t capacity_packets() const noexcept;
    [[nodiscard]] std::uint32_t next_sequence() const noexcept;

private:
    struct Slot {
        std::uint32_t sequence = 0;
        bool valid = false;
    };

    std::span<std::byte> slot_payload(std::size_t index)
    {
        return { storage_.data() + index * payload_size_, payload_size_ };
    }
    std::span<const std::byte> slot_payload(std::size_t index) const
    {
        return { storage_.data() + index * payload_size_, payload_size_ };
    }

    // 有符号差值比较，正确处理 sequence 回绕
    static int32_t seq_diff(std::uint32_t a, std::uint32_t b) noexcept
    {
        return static_cast<int32_t>(a - b);
    }

    // 以指定 sequence 为基准初始化播放时间线（首个包或 reset 后）
    void init_timeline(std::uint32_t sequence,
        std::span<const std::byte> payload);

    AudioFormat format_;
    std::size_t payload_size_; // 每个 packet 的 PCM 字节数
    std::chrono::microseconds packet_duration_; // 每包时长（由 frames_per_packet 和 sample_rate 推导）

    std::size_t target_latency_packets_;
    std::size_t capacity_;
    std::size_t slot_mask_;

    std::vector<Slot> slots_;
    std::vector<std::byte> storage_;
    mutable std::mutex slots_mutex_; // 保护 slots_ / next_pop_seq_ / highest_pushed_seq_ 跨线程读取

    // 播放时间线
    bool initialized_ = false; // 是否收到第一个包
    std::uint32_t next_pop_seq_ = 0; // 下一个期望 pop 的 sequence
    std::uint32_t highest_pushed_seq_ = 0; // 已 push 的最高 sequence
    time_point first_packet_time_ { }; // 第一个包到达时间
    time_point next_deadline_ { }; // 下一个 pop 的 deadline

    // 连续 late 包计数：用于检测音频源暂停后恢复导致的时间线失步。
    // 当 pop 空转推进 next_pop_seq_ 超前于实际到达的包时，新包全部判为 late（diff<0），
    // 永远无法触发 diff>=capacity 的 reset。连续 late 达到阈值时强制 reset 重建时间线。
    // 非 late 包（expected/future）复位此计数。
    std::uint32_t consecutive_late_ = 0;

    // 漂移检测：滑动窗口内 late 包比例超过阈值时 rebase 时间线。
    // 与 consecutive_late_ 互补：consecutive_late_ 检测全部 late（暂停/恢复），
    // 窗口比例检测交替 late（时钟漂移）。
    std::uint32_t drift_window_size_;
    std::uint32_t drift_late_threshold_;
    std::uint32_t drift_late_count_ = 0;
    std::uint32_t drift_total_count_ = 0;

    // 统计（reset 不清除，仅累积）。atomic 允许诊断 getter 从其他线程安全读取。
    std::atomic<std::uint64_t> packets_received_ { 0 };
    std::atomic<std::uint64_t> packets_lost_ { 0 };
    std::atomic<std::uint64_t> duplicates_ { 0 };
    std::atomic<std::uint64_t> late_packets_ { 0 };
};

} // namespace aqua::jitter

#endif // AQUA_JITTER_BUFFER_H
