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
//   - 启动 pre-roll：lead 达到 startup_level（默认 50%）即建立 anchor 开始播放——锚定即通知
//     音频线程消费，为涌入中的帧留 headroom（等 target 会把低容量 JB 打满）；
//
// pull 路径禁止锁 / 堆分配 / 系统调用；所有预分配在构造（控制线程）完成。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_frame.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace aqua::audio {

inline constexpr std::uint32_t JITTER_BUFFER_MIN_CAPACITY_SLOTS = 4;

// ---- 可调策略常量（语义见 doc/buffer_design.md）----
// reanchor 请求允许的最大序列跨度（帧）：超过即判定为荒谬请求并拒绝（sanity）。
inline constexpr std::uint64_t JITTER_BUFFER_MAX_REANCHOR_JUMP_FRAMES = 100'000;
// reanchor 后水位卡死的兜底：连续该次数 pull 内水位无进展则强制放弃 hold。
inline constexpr std::uint32_t JITTER_BUFFER_REANCHOR_HOLD_STUCK_PULLS = 5;
// 远超前 reanchor 的最小缺口（包）：s 与 highest 的间隔小于该值视为顺序溢出
// （ring 满后 producer 短暂无法落盘，highest 冻结），应由 deadline-high DROP
// 兜底而非 reanchor；只有缺口明显更大（时间线断裂）才 reanchor。值太小会把
// 正常满窗误判为断裂（reanchor 风暴），太大则漏掉真正的中断跳变。
inline constexpr std::uint32_t JITTER_BUFFER_REANCHOR_MIN_GAP = 4;
// max_step=0 自动推导：max(AUTO_MAX_STEP_MIN, round(AUTO_MAX_STEP_FRACTION × N))。
inline constexpr std::uint32_t JITTER_BUFFER_AUTO_MAX_STEP_MIN = 2;
inline constexpr double JITTER_BUFFER_AUTO_MAX_STEP_FRACTION = 0.10;
// 默认 warning 步长曲线：连续该次数 warning 评估才按 growth 增长一级。
inline constexpr std::uint32_t JITTER_BUFFER_WARNING_GROWTH_INTERVAL = 4;

// warning 递增步长参数与可插拔步长函数（步长单位：slot）。
struct WarningStepParams {
    std::uint32_t min_step = 1; // 起始步长（槽）
    std::uint32_t max_step = 0; // 0 = 自动：见 JITTER_BUFFER_AUTO_MAX_STEP_*
    double growth = 2.0; // 每连续评估一次的倍率
};

// 返回本次调整步长（槽数）。k = 连续处于 warning 的评估次数（≥1）。
// 默认曲线为每 4 次连续评估才按 growth 增长一级，并始终受 max_step 限制。
// pull() / decide() 属于实时路径：步长函数必须是无状态、无分配、noexcept 的函数指针。
// 若未来需要带状态策略，应把状态作为 JitterBuffer 的预分配成员，而不是捕获对象。
using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, std::uint32_t) noexcept;

// 默认实现：调用方已将 max_step=0 规范化为具体上限后，计算
// step = min(cap, base × growth^floor((k−1)/4))。
std::uint32_t default_warning_step(const WarningStepParams&, std::uint32_t k) noexcept;

// Phase 2 PCM concealment（细则 §9 / §14）：缺帧时用"重复上一个有效包 + 短淡出"
// 代替硬静音，超过连续上限后转静音。
//
// 只改输出内容，不改 sequence/timestamp，不参与 target/estimator 的统计语义。
// 第一版只做 PCM 能合理做到的事：整包重复 + 线性淡出 + 静音封顶，不做 pitch
// 估计、不做波形拼接等复杂 DSP。
struct ConcealmentConfig {
    // 组件默认关：JitterBuffer 作为可复用组件保持 v1 静音行为，产品默认由
    // ClientRuntime（ClientRuntimeConfig::pcm_concealment）决定。
    bool enabled = false;
    // 连续掩盖上限（包）。第 i 个被掩盖的包增益 = (max - i) / max，
    // 第 max+1 个起转静音。0 视为关闭。
    std::uint32_t max_slots = 3;
};

struct JitterBufferConfig {
    // N：环形槽数。三层同义：本字段 = ClientRuntimeConfig::jb_capacity_slots
    // = C API jb_capacity_slots（CLI --jb-capacity）。内层不带 jb_ 前缀是
    // 有意的——JitterBuffer 是可复用组件，不该感知客户端配置层的命名。
    std::uint32_t capacity_slots = 30;
    AudioFormat format; // 权威格式（必填）
    std::uint32_t frame_count = 0; // F：每 AudioFrame 的 sample frame 数（必填，来自 server）

    ConcealmentConfig concealment; // Phase 2 PCM concealment（默认关 = v1 静音）

    double target = 0.60; // 恢复目标 / 稳态中心
    double normal_low = 0.35; // normal 下界
    double normal_high = 0.80; // normal 上界
    double warning_low = 0.20; // warning/deadline 下分界
    double warning_high = 0.90; // warning/deadline 上分界

    // 启动 pre-roll 水位：lead 达到该水位即锚定并通知音频线程开始消费。
    // 独立于稳态阈值序，可调（0,1]。默认 50%：早于 target 锚定给涌入
    // 中的帧留 headroom（等 target 会在通知间隙被网络推入打满低容量
    // JB → deadline-high Drop 抽搐），又高于 normal_low 提供足够的
    // 抗抖动垫层；锚定后 lead 位于 normal 区，稳态自然向 target 漂移。
    double startup_level = 0.50;

    WarningStepParams step;
    WarningStepFn step_fn = &default_warning_step;
};

struct JitterBufferPullResult {
    std::uint32_t frames_filled = 0; // 本次实际填充帧数（== 请求帧数）
    std::uint32_t silence_frames = 0; // 其中静音帧数（缺帧 + 低水位强制静音 Hold）
    std::uint32_t skipped_slots = 0; // 本次跳过的槽数（Drop）
};

// 当前时间轴修正方向（诊断 Gauge）。与内部 EpisodeDir 一一对应，但可跨线程读
// （原子镜像），用于回答"此刻 JB 是否正在主动修正时间轴"。
enum class JitterBufferEpisodeState : std::uint8_t {
    None = 0, // 稳态（normal 区）
    Filling = 1, // 低水位：重复/静音停住以减慢播放
    Dropping = 2, // 高水位：跳过整槽以追上
};

[[nodiscard]] constexpr const char* jitter_buffer_episode_state_name(
    JitterBufferEpisodeState state) noexcept
{
    switch (state) {
    case JitterBufferEpisodeState::None:
        return "none";
    case JitterBufferEpisodeState::Filling:
        return "filling";
    case JitterBufferEpisodeState::Dropping:
        return "dropping";
    }
    return "unknown";
}

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

    // ---- Phase 2 欠载预算（细则 §8/§11）----
    // underrun = "播放头推进到没有真实 PCM 可用的 slot"，与静音不是一回事：
    // concealment 开启后这些帧被 repeat-last 掩盖（计入 underrun，不计入 silence）；
    // pre-roll 与低水位 Hold 的静音是时间轴修正，不计入 underrun。
    [[nodiscard]] std::uint64_t underrun_events() const noexcept { return underrun_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t underrun_frames() const noexcept { return underrun_frames_.load(std::memory_order_relaxed); }
    // 最长连续缺帧 slot run（单次 underrun 的包数），验收"单次≤3 包"用。
    [[nodiscard]] std::uint64_t max_consecutive_underrun_slots() const noexcept { return max_consecutive_underrun_slots_.load(std::memory_order_relaxed); }
    // ---- Phase 2 concealment 计数（细则 §9/§14）----
    [[nodiscard]] std::uint64_t concealed_slots() const noexcept { return concealed_slots_.load(std::memory_order_relaxed); }
    // 超过连续上限、退回静音的 slot 数（conceal 被封顶的次数）。
    [[nodiscard]] std::uint64_t concealed_saturated_slots() const noexcept { return concealed_saturated_slots_.load(std::memory_order_relaxed); }
    // 迟到包里"本可用"的数量：落后播放头不超过 conceal 窗口（max_slots），
    // 即它到达时对应的 slot 还在被掩盖——用于判断 late/reorder 值不值得接
    // （细则 §14：本阶段继续 drop，只记录 usefulness potential）。
    [[nodiscard]] std::uint64_t late_useful_packets() const noexcept { return late_useful_packets_.load(std::memory_order_relaxed); }

    // ---- 时间轴位置（Gauge：water_level 是归一化的，lead/sequence 才是绝对值）----
    // lead_slots = highest - play + 1（未锚定时以 oldest 代 play，与 water_level 同口径）。
    // water=0.48 且 lead=1 与 water=0.48 且 lead=40 是完全不同的两件事：
    // 前者说明大量 slot 分布在 playhead 后面的空洞里（sparse JB 尤其重要）。
    [[nodiscard]] std::uint32_t lead_slots() const noexcept;
    // 播放头序列（未锚定 = 0）。highest_received_sequence 为已收到的最高序列。
    [[nodiscard]] std::uint64_t play_sequence() const noexcept;
    [[nodiscard]] std::uint64_t highest_received_sequence() const noexcept;
    // 一次 target 快照下的一组水位带（槽）。四个值同源：诊断要解释「JB 为什么
    // 没达到 target」时应整体取用 bands()，不要连着读四个单值——target 可能在
    // 这期间被 producer 改写，四次读会来自不同快照。
    struct Bands {
        std::uint32_t warning_low = 0;
        std::uint32_t normal_low = 0;
        std::uint32_t normal_high = 0;
        std::uint32_t warning_high = 0;
    };
    // 当前 target 下的四个水位带（同一快照）。
    [[nodiscard]] Bands bands() const noexcept;

    // Phase 1 自适应 target 读（push strand 写 set_target_slots，RT/诊断读）。
    [[nodiscard]] std::uint32_t target_slots() const noexcept
    {
        return target_slots_.load(std::memory_order_relaxed);
    }
    // Phase 1 自适应 target 写入（push strand 写，RT/诊断读；钳制 [1, capacity]）。
    // 只写 target 一个原子：四个带值由构造期预计算的带表按 target 现算，不存在
    // 「target 已更新、band 还是旧值」的中间态。
    void set_target_slots(std::uint32_t slots) noexcept;
    // 单个水位带（槽，只读）。只关心某一个带时用；要同时看多个带请用 bands()。
    [[nodiscard]] std::uint32_t warning_low_slots() const noexcept
    {
        return band_slots(target_slots(), BandWarningLow);
    }
    [[nodiscard]] std::uint32_t normal_low_slots() const noexcept
    {
        return band_slots(target_slots(), BandNormalLow);
    }
    [[nodiscard]] std::uint32_t normal_high_slots() const noexcept
    {
        return band_slots(target_slots(), BandNormalHigh);
    }
    [[nodiscard]] std::uint32_t warning_high_slots() const noexcept
    {
        return band_slots(target_slots(), BandWarningHigh);
    }

    // ---- 断流的"形状"（pull_silence_frames 只给累计值，分不出形状）----
    // silence_frames=500 既可能是 500 个独立的 1-frame gap（网络抖动，可接受），
    // 也可能是连续 500 帧 blackout（瞬断/设备切换/UDP burst loss，严重）。
    // consecutive = 当前连续静音 run（非静音归零）；max_run = 本次运行最长 run。
    [[nodiscard]] std::uint64_t consecutive_silence_frames() const noexcept
    {
        return consecutive_silence_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t max_silence_run_frames() const noexcept
    {
        return max_silence_run_frames_.load(std::memory_order_relaxed);
    }

    // ---- 当前 episode 状态（Gauge）----
    // fill_episodes/drop_episodes 是历史计数；这两个回答"此刻是否正在修正"。
    [[nodiscard]] JitterBufferEpisodeState episode_state() const noexcept
    {
        return static_cast<JitterBufferEpisodeState>(
            episode_state_.load(std::memory_order_relaxed));
    }

    // ---- reanchor 的待处理状态 ----
    // reanchor_requests=20 但 reanchor_count=3 时，无法区分"20 次请求最终都取消了"
    // 与"还有请求在排队等 consumer 应用"。pending/target 补齐这一信息。
    [[nodiscard]] bool reanchor_pending() const noexcept;
    // 待应用的目标序列（无待处理 = 0）。
    [[nodiscard]] std::uint64_t reanchor_target_sequence() const noexcept;

    // 复位到未启动态。要求 producer / consumer 两侧均已停止（文档约定）。
    void reset() noexcept;

private:
    explicit JitterBuffer(const JitterBufferConfig& config);

    struct SlotHeader; // 定义见 .cpp

    // 固定配置
    std::uint32_t capacity_ = 0;
    std::uint32_t frame_count_ = 0;
    std::uint32_t frame_bytes_ = 0;
    std::byte silence_byte_ { 0 };
    std::size_t slot_bytes_ = 0;
    std::size_t capacity_bytes_ = 0;
    // Phase 2 concealment 需要按样本缩放（淡出），因此保留编码几何：
    // 只在构造期读取，pull 路径无分支开销之外的额外成本。
    AudioFormat format_;
    std::uint32_t bytes_per_sample_ = 0;
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
    // Phase 2 欠载预算 / concealment / late usefulness（细则 §8 §9 §11 §14）。
    // 写者单一（underrun_* 与 concealed_* 由 consumer RT 写，late_useful 由
    // producer 写），诊断线程 relaxed 读，故用原子而非普通字段。
    std::atomic<std::uint64_t> underrun_events_ { 0 };
    std::atomic<std::uint64_t> underrun_frames_ { 0 };
    std::atomic<std::uint64_t> max_consecutive_underrun_slots_ { 0 };
    std::atomic<std::uint64_t> concealed_slots_ { 0 };
    std::atomic<std::uint64_t> concealed_saturated_slots_ { 0 };
    // 迟到包里本可用的数量（producer 写，诊断线程 relaxed 读）：见上方 §14 说明。
    std::atomic<std::uint64_t> late_useful_packets_ { 0 };
    // 断流形状（consumer 写，诊断线程 relaxed 读）
    std::atomic<std::uint64_t> consecutive_silence_frames_ { 0 };
    std::atomic<std::uint64_t> max_silence_run_frames_ { 0 };
    // 当前 episode 方向的跨线程镜像（与 consumer 私有的 episode_dir_ 同步更新）
    std::atomic<std::uint8_t> episode_state_ { 0 };

    std::atomic<std::uint64_t> reanchor_request_seq_;
    std::atomic<std::uint64_t> reanchor_count_;
    std::atomic<std::uint64_t> reanchor_sanity_rejections_;
    std::atomic<std::uint64_t> reanchor_sanity_pending_ { 0 };
    std::atomic<std::uint64_t> last_reanchor_sequence_;

    // consumer 独占状态（实时线程内）
    std::uint32_t read_offset_ = 0;
    bool current_slot_ready_ = false;
    enum class EpisodeDir : std::uint8_t { None,
        Up,
        Down };
    EpisodeDir episode_dir_ = EpisodeDir::None;
    std::uint32_t consecutive_warning_ = 0;
    std::uint32_t fill_repeat_slots_remaining_ = 0; // 尚未开始重播的 slot 数（warning 区）
    bool fill_replaying_current_slot_ = false; // 当前 slot 是否处于 warning FILL 重播阶段
    bool hold_until_target_ = false;
    // 延迟的 reanchor 目标（consumer 私有）。原子化只为让 reanchor_pending()
    // 能被诊断线程读取；读写都在 consumer 线程，故一律 relaxed。
    std::atomic<std::uint64_t> deferred_reanchor_seq_ {
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t last_hold_lead_ = 0;
    std::uint32_t hold_stuck_pulls_ = 0;

    // ---- Phase 2 concealment 消费侧私有状态（全部只在 RT consumer 线程写）----
    bool conceal_enabled_ = false; // 归一化后的开关（config.enabled && max_slots > 0）
    std::uint32_t conceal_max_slots_ = 0; // 连续掩盖上限（包）
    std::vector<std::byte> last_pcm_; // 上一个真实 slot 的 PCM 副本（构造期预分配 slot_bytes_）
    bool have_last_pcm_ = false; // 从未播过真实 PCM / reanchor 后作废
    bool conceal_active_ = false; // 当前 slot 正在被 repeat-last 掩盖
    std::uint32_t conceal_gain_q15_ = 0; // 当前掩盖 slot 的增益（Q15，32768 = 1.0）
    std::uint32_t conceal_run_slots_ = 0; // 当前连续掩盖 slot 数（出现真实数据归零）
    std::uint32_t underrun_run_slots_ = 0; // 当前连续缺帧 slot 数（出现真实数据归零）
    bool underrun_active_ = false; // 当前是否处于"无真实 PCM"状态（event 边沿检测）

    // 构造时预计算的整数阈值
    std::uint32_t startup_slots_ = 0;
    // Phase 1 自适应 target：push strand 经 set_target_slots 写，consumer RT
    // 与诊断线程读（一写多读，relaxed 原子）。warning/normal 带以 target 为
    // 基准按 create 时的比例跟随（存 multiplier，不存绝对值），保证 target
    // 降到个位数时带区间不脱钩；固定模式（从不调 set）下与旧行为逐值一致。
    std::atomic<std::uint32_t> target_slots_ { 1 };
    double band_wl_per_target_ = 0.0;
    double band_nl_per_target_ = 0.0;
    double band_nh_per_target_ = 0.0;
    double band_wh_per_target_ = 0.0;
    // 水位带查找表：band_table_[target * BandCount + Band*]，target ∈ [0, capacity_]。
    // 构造期一次性预计算，此后只读。带值不再各自存成原子，而是由「本拍 target」
    // 查表现算——decide() 与诊断因此只能读到同一 target 下的一组带，从根上消除
    // 旧实现「target 与四个带分五次独立 store、RT 侧分五次读」的撕裂窗口。
    std::vector<std::uint32_t> band_table_;

    // 可插拔步长
    WarningStepParams step_params_;
    WarningStepFn step_fn_ = nullptr;

    // 带表列号（band_table_ 的第二维下标）。
    enum : std::uint32_t {
        BandWarningLow = 0,
        BandNormalLow,
        BandNormalHigh,
        BandWarningHigh,
        BandCount
    };
    // 查表：target（调用方保证已在 [0, capacity_]）→ 该带的槽数。无浮点、
    // 无分配、无锁，RT 安全。
    [[nodiscard]] std::uint32_t band_slots(std::uint32_t target, std::uint32_t which) const noexcept
    {
        const std::uint32_t t = target > capacity_ ? capacity_ : target;
        return band_table_[static_cast<std::size_t>(t) * BandCount + which];
    }
    // 给定 target 快照一次算出四个带（decide() 与 bands() 共用，保证同源）。
    [[nodiscard]] Bands bands_from(std::uint32_t target) const noexcept
    {
        Bands b;
        b.warning_low = band_slots(target, BandWarningLow);
        b.normal_low = band_slots(target, BandNormalLow);
        b.normal_high = band_slots(target, BandNormalHigh);
        b.warning_high = band_slots(target, BandWarningHigh);
        return b;
    }

    [[nodiscard]] std::byte* slot_data(std::uint32_t idx) noexcept;

    void snapshot_current() noexcept;
    void advance_slot() noexcept;
    void end_episode() noexcept;
    // episode_dir_ 的原子镜像同步（consumer 线程内调用，relaxed 写）。
    void publish_episode_state(EpisodeDir dir) noexcept
    {
        const auto value = dir == EpisodeDir::Up
            ? JitterBufferEpisodeState::Filling
            : (dir == EpisodeDir::Down ? JitterBufferEpisodeState::Dropping
                                       : JitterBufferEpisodeState::None);
        episode_state_.store(static_cast<std::uint8_t>(value), std::memory_order_relaxed);
    }
    // 静音 run 结算：连续静音累加并刷新 max，出现真实数据即归零。
    void record_silence_run(std::uint32_t silence_frames) noexcept;
    void request_reanchor(std::uint64_t sequence) noexcept;
    void apply_reanchor(std::uint64_t sequence) noexcept;

    // ---- Phase 2 concealment（RT consumer 线程内调用）----
    // 槽边界判定：本 slot 有真实数据 → 复位连续计数；无数据 → 进入缺帧处理，
    // 决定"repeat-last 掩盖 / 静音"，并结算 underrun run 与 counters。
    void on_slot_boundary(bool ready) noexcept;
    // 进入"无真实 PCM"状态（边沿计数）：缺帧 slot 与"JB 排空"守卫路径都走这里。
    // 排空路径（play > highest）不推进 slot，故只记 event，不计 slot run。
    void mark_underrun() noexcept;
    // 当前掩盖 slot 的线性淡出增益：(max - i) / max，i = 已连续掩盖的 slot 数。
    [[nodiscard]] std::uint32_t conceal_gain_for(std::uint32_t run_index) const noexcept;
    // 把 frames 个 sample frame 按 Q15 增益原地缩放（各 PCM 编码逐样本处理）。
    void scale_frames(std::byte* dst, std::uint32_t frames, std::uint32_t gain_q15) const noexcept;
    // 迟到包（producer 侧调用）：lateness_slots = 落后播放头的 slot 数。
    void note_late_packet(std::uint64_t lateness_slots) noexcept;
    // Hold：warning 区表示慢放重播；hold_until_target_ 下表示低水位强制静音。
    enum class Action : std::uint8_t { None,
        Hold,
        Skip };
    [[nodiscard]] Action decide(std::uint64_t lead, std::uint32_t& skip_step) noexcept;
    [[nodiscard]] std::uint32_t clamp_step(std::uint32_t raw) const noexcept;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_BUFFER_JITTER_BUFFER_H
