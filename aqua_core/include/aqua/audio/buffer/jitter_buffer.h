#ifndef AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
#define AQUA_AUDIO_BUFFER_JITTER_BUFFER_H

// JitterBuffer：客户端接收/回放路径上唯一的缓冲。
//
// 模型（详见 doc/buffer_design.md）：
//   - 环形固定容量 N 个 slot，每个 slot 存一个完整 AudioFrame（定长 F 个 sample frame）；
//   - 以 AudioFrame::sequence 排序/定位；播放节奏由本地回放时钟驱动；
//   - producer = 网络线程（push），consumer = 回放实时线程（pull），SPSC 无锁；
//   - 水位 W = lead_slots / N，lead_slots = highest_seq - play_seq + 1；
//   - 低水位 Fill（补静音、play_seq 不动），高水位 Drop（跳过整槽），缺帧补 F 帧静音；
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
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace aqua::audio {

// warning 递增步长参数与可插拔步长函数（步长单位：slot）。
struct WarningStepParams {
    std::uint32_t min_step = 1;   // 起始步长（槽）
    std::uint32_t max_step = 0;   // 0 = 自动：max(2, round(0.10 × N))
    double growth = 2.0;          // 每连续评估一次的倍率
};

// 返回本次调整步长（槽数）。k = 连续处于 warning 的评估次数（≥1）。
// 用 std::function 而非裸函数指针，以支持带状态的自定义步长策略。
using WarningStepFn = std::function<std::uint32_t(const WarningStepParams&, std::uint32_t)>;

// 默认实现：step = min(cap, base × growth^(k−1))。
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
    std::uint32_t silence_frames = 0;  // 其中静音帧数（缺帧 + Fill）
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

    // producer（网络线程）。true = 接受；false = 丢弃（大小不符/迟到/越界/重复/冲突）。
    bool push(const AudioFrame& frame) noexcept;

    // consumer（回放实时线程）。按 output 容量填充（真实数据 + 静音），不阻塞、不加锁、不分配。
    JitterBufferPullResult pull(std::span<std::byte> output) noexcept;

    // 诊断/测试接口（bytes 不参与水位控制）。
    [[nodiscard]] std::uint32_t capacity_slots() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t used_slots() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    [[nodiscard]] std::size_t used_bytes() const noexcept;
    [[nodiscard]] double water_level() const noexcept;

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

    // consumer 独占状态（实时线程内）
    std::uint32_t read_offset_ = 0;
    bool current_slot_ready_ = false;
    enum class EpisodeDir : std::uint8_t { None, Up, Down };
    EpisodeDir episode_dir_ = EpisodeDir::None;
    std::uint32_t consecutive_warning_ = 0;
    std::uint32_t hold_remaining_ = 0;   // sample 帧
    bool hold_until_target_ = false;

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

    enum class Action : std::uint8_t { None, Hold, Skip };
    [[nodiscard]] Action decide(std::uint64_t lead, std::uint32_t& skip_step) noexcept;
    [[nodiscard]] std::uint32_t clamp_step(std::uint32_t raw) const noexcept;
    [[nodiscard]] std::uint32_t hold_frames(std::uint32_t raw_step) const noexcept;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
