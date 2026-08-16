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
// - rebase 保持节奏：小缺口沿原 cadence 推进 deadline（PLC 填补），
//   只有大于 target 的断裂才重新缓冲，避免每次 rebase 停供打穿下游 RB
// - 自适应 target（可选）：构造 floor 为下限兼初始值，late 压力下抬升、
//   持续干净后缓慢回落，区间 [floor, ceiling]。调节通过 next_deadline_ ±1 拍
//   实现蓄水/排水；与 drift rebase（时间线级）共用检测窗口但机制正交。
//
// Threading contract:
//   push() 和 pop_next() 必须在同一个 executor / 线程中调用。
//   当前设计为 io_context 单线程，push 来自 UDP 回调，pop_next 来自 steady_timer 回调。
//   slots_ 访问由 slots_mutex_ 保护，允许诊断 getter（buffer_fill_packets）从其他线程安全读取。
//   统计计数器（packets_received_ 等）为 atomic，可从任意线程读取。

// 自适应 target 参数（默认值取 config.h）。快升慢降 + 迟滞带：
//   窗口内 late >= raise_late_count        → target +1 包（deadline 后移 1 拍蓄水）
//   连续 lower_clean_windows 个干净窗口    → target -1 包（deadline 前移 1 拍排水）
//   0 < late < raise_late_count            → 保持，且打断连续干净计数
// max_packets（可选）：自适应 target 上限（包数）。nullopt = capacity/2（默认）。
// 调用方给大 ceiling 时需同步放大 capacity（构造校验 max_packets <= capacity/2）；
// 上游推导规则见 config.h（用户面仅 jitter-buffer 单参数：capacity = bit_ceil(ms)，
// floor = capacity/4，ceiling = capacity/2）。
struct AdaptiveTargetConfig {
    std::uint32_t raise_late_count = aqua::config::JITTER_DETECT_RAISE_LATE_COUNT;
    std::uint32_t lower_clean_windows = aqua::config::JITTER_DETECT_LOWER_CLEAN_WINDOWS;
    std::optional<std::size_t> max_packets; // nullopt = capacity/2
};

class JitterBuffer {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // 构造时预分配所有内存。
    // format:           音频格式（决定 frame_bytes）
    // frames_per_packet: 每包帧数（决定 payload_size 和 packet_duration）
    // floor_packets:     起播缓冲包数（如 48kHz 下 10 包 = 30ms）；
    //                    启用自适应时同时是 target 的下限
    // capacity_packets:  ring 容量，必须为 2 的幂，>= floor_packets * 2
    //                    （target 上限 capacity/2 或 AdaptiveTargetConfig.max_packets）
    // detect_window_packets: 检测窗口大小（包数），drift rebase 与 AIMD 共用，默认 config.h 值
    // drift_rebase_late_count: 窗口内 late 包数 >= 此值时触发时间线 rebase，默认 config.h 值
    // adaptive:          启用自适应 target（nullopt = 固定 target，库默认关闭保证行为确定）
    JitterBuffer(const AudioFormat& format,
        std::uint32_t frames_per_packet,
        std::size_t floor_packets,
        std::size_t capacity_packets,
        std::uint32_t detect_window_packets = aqua::config::JITTER_DETECT_WINDOW_PACKETS,
        std::uint32_t drift_rebase_late_count = aqua::config::JITTER_DRIFT_REBASE_LATE_COUNT,
        std::optional<AdaptiveTargetConfig> adaptive = std::nullopt);

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;

    // UDP I/O 线程调用：推入收到的音频包。
    // 自动归类：expected / future / duplicate / late。
    // push 时不判定丢包。payload 大小不匹配的畸形包计数后丢弃（malformed_packets）。
    void push(std::uint32_t sequence,
        std::span<const std::byte> payload);

    // 外部调度器查询下一次播放 deadline。
    // 返回 nullopt 表示尚未收到第一个包。
    // 线程契约：必须在调用 push()/pop_next() 的同一线程调用（当前 io_context 单线程模型）。
    // 本方法无锁读取时间线状态，跨线程调用是 data race；诊断用途请用带锁的
    // buffer_fill_packets()/next_sequence()。
    [[nodiscard]] std::optional<time_point> next_playout_deadline() const noexcept;

    // deadline 到达后调用。输出 payload_size 字节：真实 PCM 或丢包隐藏。
    // 丢包时输出"上一包 PCM 的衰减重复"（Packet Loss Concealment），
    // 连续丢包每包增益减半（0.5, 0.25, ...）收敛为静音；无上一包历史时直接静音。
    // 返回 true 表示输出了真实 PCM，false 表示输出了隐藏/静音（丢包）。
    // 长时间断流（deadline 落后超过整个 target 缓冲量）时重置时间线并输出静音。
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
    [[nodiscard]] std::uint64_t malformed_packets() const noexcept;
    [[nodiscard]] std::uint64_t rebases() const noexcept; // 时间线重建次数（drift/跳跃/连续late 触发）
    [[nodiscard]] std::size_t target_latency_packets() const noexcept; // 当前 target（自适应时可变，加锁读）
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

    // reset 的无锁体（pop_next 内部断流检测复用，调用方必须已持有 slots_mutex_）
    void reset_playout_state_locked();

    // 检测窗口评估：在 push 完成当前包分类并计入窗口后调用。
    // 窗口满时用同一份计数依次评估：drift rebase（late >= drift_rebase_late_count_
    // → init_timeline 重建时间线）与 AIMD（raise/lower/hold），然后重置窗口。
    // 返回 true 表示 drift rebase 已接管本包（init_timeline 已存储），调用方跳过常规入槽。
    // 调用方必须已持有 slots_mutex_ 且时间线已初始化。
    [[nodiscard]] bool evaluate_detect_window_locked(std::uint32_t sequence,
        std::span<const std::byte> payload);

    // 丢包隐藏：按编码逐样本乘增益（S16/S32/F32）。
    // S24LE/U8 解包成本高且极少使用，输出静音（全零）。
    void apply_gain(std::span<std::byte> pcm, float gain) const noexcept;

    AudioFormat format_;
    std::size_t payload_size_; // 每个 packet 的 PCM 字节数
    std::chrono::nanoseconds packet_duration_; // 每包时长（纳秒精度，由 frames_per_packet 和 sample_rate 推导）

    std::size_t target_latency_packets_; // 当前 target（自适应启用时在 [floor, ceiling] 游走）
    std::size_t floor_packets_; // 自适应下限（= 构造时的 floor）；未启用时恒等于 target
    bool adaptive_; // 自适应开关（拷贝一份避免 optional 重复解引用）
    AdaptiveTargetConfig adapt_cfg_;
    std::uint32_t adapt_clean_streak_ = 0; // 连续干净窗口数（rebase 时清零）

    std::size_t capacity_;
    std::size_t slot_mask_;

    std::vector<Slot> slots_;
    std::vector<std::byte> storage_;
    mutable std::mutex slots_mutex_; // 保护 slots_ / next_pop_seq_ / highest_pushed_seq_ 跨线程读取

    // 丢包隐藏（PLC）：上一包真实 PCM 的副本 + 当前隐藏增益。
    // pop 真实包时刷新副本并置增益 1.0；每次隐藏输出后增益减半，收敛为静音。
    // 0.0 表示无可用历史（reset 后），隐藏路径输出纯静音。
    std::vector<std::byte> last_pcm_;
    float hide_gain_ = 0.0f;

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

    // 检测窗口：统计有效到达包（排除重复/畸形）中的 late 数，窗口满时
    // 由 evaluate_detect_window_locked 评估（drift rebase 与 AIMD 共用同一份计数）。
    // 与 consecutive_late_ 互补：consecutive_late_ 检测全部 late（暂停/恢复），
    // 窗口比例检测交替 late（时钟漂移 / 网络压力）。
    std::uint32_t detect_window_packets_;
    std::uint32_t drift_rebase_late_count_;
    std::uint32_t window_late_count_ = 0; // 当前窗口 late 数
    std::uint32_t window_total_count_ = 0; // 当前窗口有效到达数

    // 统计（reset 不清除，仅累积）。atomic 允许诊断 getter 从其他线程安全读取。
    std::atomic<std::uint64_t> packets_received_ { 0 };
    std::atomic<std::uint64_t> packets_lost_ { 0 };
    std::atomic<std::uint64_t> duplicates_ { 0 };
    std::atomic<std::uint64_t> late_packets_ { 0 };
    std::atomic<std::uint64_t> malformed_packets_ { 0 }; // payload 大小不匹配的畸形包
    std::atomic<std::uint64_t> rebases_ { 0 }; // 已初始化后的时间线重建次数（不含首包）
};

} // namespace aqua::jitter

#endif // AQUA_JITTER_BUFFER_H
