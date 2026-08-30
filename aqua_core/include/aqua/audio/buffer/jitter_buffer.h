#ifndef AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
#define AQUA_AUDIO_BUFFER_JITTER_BUFFER_H

// JitterBuffer：客户端接收/回放路径上唯一的缓冲。
//
// 模型（详见 doc/buffer_design.md）：
//   - 环形固定容量 N 个 slot，每个 slot 存一个完整 AudioFrame（定长 F 个 sample frame）；
//   - 以 AudioFrame::sequence 排序/定位；播放节奏由本地回放时钟驱动；
//   - producer = 网络线程（push），consumer = 回放实时线程（pull），SPSC 无锁；
//   - 水位 W = lead_slots / N，lead_slots = highest_seq - play_seq + 1；
//   - 低水位 Fill（warning 区重复当前 READY slot 以减慢播放），高水位 Drop（跳过整槽），缺帧补 F 帧静音；
//   - 启动 pre-roll：lead 达到 60% 才建立 anchor 开始播放。
//
// pull 路径禁止锁 / 堆分配 / 系统调用；所有预分配在构造（控制线程）完成。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <limits>
#include <span>
#include <vector>

namespace aqua::audio {

inline constexpr std::uint32_t JITTER_BUFFER_MIN_CAPACITY_SLOTS = 4;

// ---- 可调策略常量（语义见 doc/buffer_design.md）----
// reanchor 请求允许的最大序列跨度（帧）：超过即判定为荒谬请求并拒绝（sanity）。
inline constexpr std::uint64_t JITTER_BUFFER_MAX_REANCHOR_JUMP_FRAMES = 100'000;
// reanchor 后水位卡死的兜底：连续该次数 pull 内水位无进展则强制放弃 hold。
inline constexpr std::uint32_t JITTER_BUFFER_REANCHOR_HOLD_STUCK_PULLS = 5;
// max_step=0 自动推导：max(AUTO_MAX_STEP_MIN, round(AUTO_MAX_STEP_FRACTION × N))。
inline constexpr std::uint32_t JITTER_BUFFER_AUTO_MAX_STEP_MIN = 2;
inline constexpr double JITTER_BUFFER_AUTO_MAX_STEP_FRACTION = 0.10;
// 默认 warning 步长曲线：连续该次数 warning 评估才按 growth 增长一级。
inline constexpr std::uint32_t JITTER_BUFFER_WARNING_GROWTH_INTERVAL = 4;

// warning 递增步长参数与可插拔步长函数（步长单位：slot）。
struct WarningStepParams {
    std::uint32_t min_step = 1;   // 起始步长（槽）
    std::uint32_t max_step = 0;   // 0 = 自动：见 JITTER_BUFFER_AUTO_MAX_STEP_*
    double growth = 2.0;          // 每连续评估一次的倍率
};

// 返回本次调整步长（槽数）。k = 连续处于 warning 的评估次数（≥1）。
// 默认曲线为每 4 次连续评估才按 growth 增长一级，并始终受 max_step 限制。
// pull() / decide() 属于实时路径：步长函数必须是无状态、无分配、noexcept 的函数指针。
// 若未来需要带状态策略，应把状态作为 JitterBuffer 的预分配成员，而不是捕获对象。
using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, std::uint32_t) noexcept;

// 默认实现：调用方已将 max_step=0 规范化为具体上限后，计算
// step = min(cap, base × growth^floor((k−1)/4))。
std::uint32_t default_warning_step(const WarningStepParams&, std::uint32_t k) noexcept;

struct JitterBufferConfig {
    std::uint32_t capacity_slots = 30;  // N：环形槽数
    AudioFormat format;                 // 权威格式（必填）
    std::uint32_t frame_count = 0;  // F：每 AudioFrame 的 sample frame 数（必填，来自 server）

    double target = 0.60;                    // 恢复目标 / 稳态中心
    double normal_low = 0.45;                // normal 下界
    double normal_high = 0.75;               // normal 上界
    double warning_low = 0.30;               // warning/deadline 下分界
    double warning_high = 0.90;              // warning/deadline 上分界

    WarningStepParams step;
    WarningStepFn step_fn = &default_warning_step;
};

struct JitterBufferPullResult {
    std::uint32_t frames_filled = 0;   // 本次实际填充帧数（== 请求帧数）
    std::uint32_t silence_frames = 0;  // 其中静音帧数（缺帧 + 低水位强制静音 Hold）
    std::uint32_t skipped_slots = 0;   // 本次跳过的槽数（Drop）
};

class JitterBuffer {
public:
    // 校验 config（capacity/format/frame_count/阈值序）并构造；非法 → InvalidArgument，
    // 分配失败 → BackendFailed。
    static std::expected<std::unique_ptr<JitterBuffer>, AudioError>
    create(const JitterBufferConfig& config);

    JitterBuffer(const JitterBuffer&) = delete;
    JitterBuffer& operator=(const JitterBuffer&) = delete;
    ~JitterBuffer();

    // producer（网络线程）。true = 接受；false = 丢弃（大小不符/迟到/槽冲突/重复，
    // 或超过 reanchor 允许的荒谬跨度）。远超前帧（s >= play_seq + N）不再直接丢弃：
    // 接受并记录 reanchor request；由 consumer 在 pull() 中选择安全时机应用。
    bool push(const AudioFrame& frame) noexcept;

    // consumer（回放实时线程）。output 必须按 frame_bytes_ 对齐；未对齐请求直接返回 0，
    // 不阻塞、不加锁、不分配。正常路径始终填满 output（真实数据 + 静音）。
    JitterBufferPullResult pull(std::span<std::byte> output) noexcept;

    // 诊断/测试接口（bytes 不参与水位控制）。统计仅描述当前 JB 实例生命周期；
    // sanity rejection 总数与 Runtime 的 pending 通知分开维护。
    [[nodiscard]] std::uint32_t capacity_slots() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t used_slots() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    [[nodiscard]] std::size_t used_bytes() const noexcept;
    [[nodiscard]] double water_level() const noexcept;
    [[nodiscard]] std::uint64_t reanchor_count() const noexcept { return reanchor_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t reanchor_sanity_rejections() const noexcept { return reanchor_sanity_rejections_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t last_reanchor_sequence() const noexcept { return last_reanchor_sequence_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t take_reanchor_sanity_rejections() noexcept { return reanchor_sanity_pending_.exchange(0, std::memory_order_acq_rel); }
    [[nodiscard]] std::uint64_t push_accepted() const noexcept { return push_accepted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t push_rejected() const noexcept { return push_rejected_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t push_rejected_late() const noexcept { return push_rejected_late_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t push_rejected_slot_busy() const noexcept { return push_rejected_slot_busy_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t push_rejected_invalid() const noexcept { return push_rejected_invalid_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t push_rejected_sanity() const noexcept { return push_rejected_sanity_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pull_calls() const noexcept { return pull_calls_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pull_frames() const noexcept { return pull_frames_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pull_silence_frames() const noexcept { return pull_silence_frames_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t fill_episodes() const noexcept { return fill_episodes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t fill_corrected_slots() const noexcept { return fill_corrected_slots_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t drop_episodes() const noexcept { return drop_episodes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t drop_skipped_slots() const noexcept { return drop_skipped_slots_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t reanchor_requests() const noexcept { return reanchor_requests_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t reanchor_cancels() const noexcept { return reanchor_cancels_.load(std::memory_order_relaxed); }

    // 复位到未启动态。要求 producer / consumer 两侧均已停止（文档约定）。
    void reset() noexcept;

private:
    explicit JitterBuffer(const JitterBufferConfig& config);

    struct SlotHeader; // 定义见 .cpp

    // 固定配置
    std::uint32_t capacity_ = 0;
    std::uint32_t frame_count_ = 0;
    std::uint32_t frame_bytes_ = 0;
    std::size_t slot_bytes_ = 0;
    std::size_t capacity_bytes_ = 0;

    // 存储：N 个槽头 + 一段连续 PCM 存储
    std::unique_ptr<SlotHeader[]> slots_;
    std::vector<std::byte> storage_;

    // producer/consumer 共享原子状态
    std::atomic<std::uint64_t> play_seq_;
    std::atomic<std::uint64_t> highest_seq_;
    std::atomic<std::uint64_t> oldest_seq_;
    std::atomic<std::uint32_t> used_slots_;

    // producer -> consumer 的单向控制 mailbox。非哨兵值表示请求
    // 重新锚定播放时间线；由 consumer 在 pull() 中应用。
    std::atomic<std::uint64_t> push_accepted_ { 0 };
    std::atomic<std::uint64_t> push_rejected_ { 0 };
    std::atomic<std::uint64_t> push_rejected_late_ { 0 };
    std::atomic<std::uint64_t> push_rejected_slot_busy_ { 0 };
    std::atomic<std::uint64_t> push_rejected_invalid_ { 0 };
    std::atomic<std::uint64_t> push_rejected_sanity_ { 0 };
    std::atomic<std::uint64_t> pull_calls_ { 0 };
    std::atomic<std::uint64_t> pull_frames_ { 0 };
    std::atomic<std::uint64_t> pull_silence_frames_ { 0 };
    std::atomic<std::uint64_t> fill_episodes_ { 0 };
    std::atomic<std::uint64_t> fill_corrected_slots_ { 0 };
    std::atomic<std::uint64_t> drop_episodes_ { 0 };
    std::atomic<std::uint64_t> drop_skipped_slots_ { 0 };
    std::atomic<std::uint64_t> reanchor_requests_ { 0 };
    std::atomic<std::uint64_t> reanchor_cancels_ { 0 };

    std::atomic<std::uint64_t> reanchor_request_seq_;
    std::atomic<std::uint64_t> reanchor_count_;
    std::atomic<std::uint64_t> reanchor_sanity_rejections_;
    std::atomic<std::uint64_t> reanchor_sanity_pending_ { 0 };
    std::atomic<std::uint64_t> last_reanchor_sequence_;

    // consumer 独占状态（实时线程内）
    std::uint32_t read_offset_ = 0;
    bool current_slot_ready_ = false;
    enum class EpisodeDir : std::uint8_t { None, Up, Down };
    EpisodeDir episode_dir_ = EpisodeDir::None;
    std::uint32_t consecutive_warning_ = 0;
    std::uint32_t fill_repeat_slots_remaining_ = 0; // 尚未开始重播的 slot 数（warning 区）
    bool fill_replaying_current_slot_ = false; // 当前 slot 是否处于 warning FILL 重播阶段
    bool hold_until_target_ = false;
    std::uint64_t deferred_reanchor_seq_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_hold_lead_ = 0;
    std::uint32_t hold_stuck_pulls_ = 0;

    // 构造时预计算的整数阈值
    std::uint32_t target_slots_ = 0;
    std::uint32_t warning_low_slots_ = 0;
    std::uint32_t normal_low_slots_ = 0;
    std::uint32_t normal_high_slots_ = 0;
    std::uint32_t warning_high_slots_ = 0;

    // 可插拔步长
    WarningStepParams step_params_;
    WarningStepFn step_fn_ = nullptr;

    [[nodiscard]] std::byte* slot_data(std::uint32_t idx) noexcept;

    void snapshot_current() noexcept;
    void advance_slot() noexcept;
    void end_episode() noexcept;
    void request_reanchor(std::uint64_t sequence) noexcept;
    void apply_reanchor(std::uint64_t sequence) noexcept;

    // Hold：warning 区表示慢放重播；hold_until_target_ 下表示低水位强制静音。
    enum class Action : std::uint8_t { None, Hold, Skip };
    [[nodiscard]] Action decide(std::uint64_t lead, std::uint32_t& skip_step) noexcept;
    [[nodiscard]] std::uint32_t clamp_step(std::uint32_t raw) const noexcept;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
