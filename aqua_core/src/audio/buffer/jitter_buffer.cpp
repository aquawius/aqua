#include "aqua/audio/buffer/jitter_buffer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace aqua::audio {

namespace {

// play_seq 的"未启动"哨兵；oldest_seq 的"尚无帧"哨兵。
constexpr std::uint64_t kNoPlaySeq = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kNoOldestSeq = std::numeric_limits<std::uint64_t>::max();

enum class SlotState : std::uint32_t {
    Empty = 0,
    Writing = 1,
    Ready = 2,
};

[[nodiscard]] std::uint32_t round_pct(double pct, std::uint32_t n) noexcept
{
    const double v = pct * static_cast<double>(n);
    if (v >= static_cast<double>(n)) {
        return n;
    }
    if (v <= 0.0) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::lround(v));
}

} // namespace

// 槽头：state（原子发布）+ sequence（普通字段，可见性由 state 的 acquire/release 建立）。
struct JitterBuffer::SlotHeader {
    std::atomic<SlotState> state { SlotState::Empty };
    std::uint64_t sequence { 0 };
};

std::uint32_t default_warning_step(const WarningStepParams& p, std::uint32_t k) noexcept
{
    const std::uint32_t base = p.min_step == 0 ? 1u : p.min_step;
    const std::uint32_t cap = p.max_step == 0 ? base : p.max_step;
    double step = static_cast<double>(base);
    for (std::uint32_t i = 1; i < k && step < static_cast<double>(cap); ++i) {
        step *= p.growth;
    }
    if (step >= static_cast<double>(cap)) {
        return cap;
    }
    const auto v = static_cast<std::uint32_t>(step);
    return v == 0 ? 1u : v;
}

namespace {

[[nodiscard]] bool config_is_valid(const JitterBufferConfig& c) noexcept
{
    if (c.capacity_slots == 0 || c.frame_count == 0) {
        return false;
    }
    if (!c.format.is_valid()) {
        return false;
    }
    const std::size_t frame_bytes = c.format.frame_bytes();
    if (frame_bytes == 0) {
        return false;
    }
    const std::size_t slot_bytes = static_cast<std::size_t>(c.frame_count) * frame_bytes;
    if (slot_bytes == 0 || slot_bytes > std::numeric_limits<std::size_t>::max() / c.capacity_slots) {
        return false;
    }
    // 阈值严格有序且落在 (0,1]。
    if (!(c.warning_low > 0.0
            && c.warning_low < c.normal_low
            && c.normal_low < c.target
            && c.target < c.normal_high
            && c.normal_high < c.warning_high
            && c.warning_high <= 1.0)) {
        return false;
    }
    // 步长参数：min_step 必须 > 0；growth 必须 >= 1.0（<1 会让步长越调越小、不递增）。
    return c.step.min_step > 0 && c.step.growth >= 1.0;
}

} // namespace

JitterBuffer::JitterBuffer(const JitterBufferConfig& config)
    : capacity_(config.capacity_slots)
    , frame_count_(config.frame_count)
    , frame_bytes_(config.format.frame_bytes())
    , slot_bytes_(static_cast<std::size_t>(config.frame_count) * config.format.frame_bytes())
    , capacity_bytes_(static_cast<std::size_t>(capacity_) * slot_bytes_)
    , slots_(std::make_unique<SlotHeader[]>(capacity_))
    , storage_(capacity_bytes_)
    , play_seq_(kNoPlaySeq)
    , highest_seq_(0)
    , oldest_seq_(kNoOldestSeq)
    , used_slots_(0)
    , step_params_(config.step)
    , step_fn_(config.step_fn ? config.step_fn : &default_warning_step)
{
    if (step_params_.max_step == 0) {
        const auto auto_max = std::max<std::uint32_t>(
            2, round_pct(0.10, capacity_));
        step_params_.max_step = auto_max;
    }
    target_slots_ = std::max<std::uint32_t>(1, round_pct(config.target, capacity_));
    warning_low_slots_ = round_pct(config.warning_low, capacity_);
    normal_low_slots_ = round_pct(config.normal_low, capacity_);
    normal_high_slots_ = round_pct(config.normal_high, capacity_);
    warning_high_slots_ = round_pct(config.warning_high, capacity_);
}

JitterBuffer::~JitterBuffer() = default;

std::expected<std::unique_ptr<JitterBuffer>, AudioError>
JitterBuffer::create(const JitterBufferConfig& config)
{
    if (!config_is_valid(config)) {
        return std::unexpected(AudioError::InvalidArgument);
    }
    try {
        return std::unique_ptr<JitterBuffer>(new JitterBuffer(config));
    } catch (const std::bad_alloc&) {
        return std::unexpected(AudioError::BackendFailed);
    }
}

std::byte* JitterBuffer::slot_data(std::uint32_t idx) noexcept
{
    return storage_.data() + static_cast<std::size_t>(idx) * slot_bytes_;
}

std::uint32_t JitterBuffer::used_slots() const noexcept
{
    return used_slots_.load(std::memory_order_relaxed);
}

std::size_t JitterBuffer::used_bytes() const noexcept
{
    return static_cast<std::size_t>(used_slots()) * slot_bytes_;
}

double JitterBuffer::water_level() const noexcept
{
    const std::uint64_t play = play_seq_.load(std::memory_order_acquire);
    const std::uint64_t highest = highest_seq_.load(std::memory_order_acquire);
    std::uint64_t lead = 0;
    if (play == kNoPlaySeq) {
        const std::uint64_t oldest = oldest_seq_.load(std::memory_order_acquire);
        lead = (oldest <= highest) ? (highest - oldest + 1) : 0;
    } else {
        lead = (play <= highest) ? (highest - play + 1) : 0;
    }
    return static_cast<double>(lead) / static_cast<double>(capacity_);
}

bool JitterBuffer::push(const AudioFrame& frame) noexcept
{
    const std::uint64_t s = frame.sequence;

    // 帧大小防御校验（server 保证固定，这里仍拒绝错误大小）。
    if (frame.frame_count != frame_count_ || frame.data.size() != slot_bytes_) {
        return false;
    }

    const std::uint64_t play = play_seq_.load(std::memory_order_acquire);
    const bool started = (play != kNoPlaySeq);

    if (started) {
        if (s < play) {
            return false; // 迟到
        }
        if (s >= play + capacity_) {
            return false; // 越界
        }
    } else {
        // 启动前：无 play_seq 窗口，改为约束序列跨度 ≤ N，避免 lead 因远端帧被撑爆。
        const std::uint64_t oldest = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest = highest_seq_.load(std::memory_order_acquire);
        if (oldest != kNoOldestSeq) {
            if (s >= oldest + capacity_) {
                return false; // 相对最老帧过于超前
            }
            if (highest >= s && highest - s >= capacity_) {
                return false; // 相对最新帧过于滞后
            }
        }
    }

    const auto idx = static_cast<std::uint32_t>(s % capacity_);
    auto& [state, sequence] = slots_[idx];

    // claim：EMPTY → WRITING
    SlotState expected = SlotState::Empty;
    if (!state.compare_exchange_strong(expected, SlotState::Writing,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        return false; // 重复或冲突
    }

    // 写 sequence + data
    sequence = s;
    std::copy(frame.data.begin(), frame.data.end(), slot_data(idx));

    // publish：WRITING → READY
    state.store(SlotState::Ready, std::memory_order_release);

    // 迟到复查：写入期间 consumer 可能已越过该 sequence，则自行回收。
    const std::uint64_t play2 = play_seq_.load(std::memory_order_acquire);
    if (started && s < play2) {
        state.store(SlotState::Empty, std::memory_order_release);
        return false;
    }

    // commit：先 READY 后更新逻辑序号（保证 consumer 见 highest≥s 时槽已 READY 或确属缺失）。
    std::uint64_t cur = highest_seq_.load(std::memory_order_relaxed);
    if (s > cur) {
        highest_seq_.store(s, std::memory_order_release);
    }
    cur = oldest_seq_.load(std::memory_order_relaxed);
    if (s < cur) {
        oldest_seq_.store(s, std::memory_order_release);
    }
    used_slots_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void JitterBuffer::snapshot_current() noexcept
{
    const std::uint64_t p = play_seq_.load(std::memory_order_relaxed);
    const std::uint32_t idx = static_cast<std::uint32_t>(p % capacity_);
    auto& slot = slots_[idx];
    current_slot_ready_ = (slot.state.load(std::memory_order_acquire) == SlotState::Ready)
        && (slot.sequence == p);
}

void JitterBuffer::advance_slot() noexcept
{
    const std::uint64_t cur = play_seq_.load(std::memory_order_relaxed);
    // 先推进 play_seq 再回收槽：若先回收，producer 可在"回收完成→play_seq 未推进"
    // 的窗口里把重复 sequence 写回刚清空的槽，而迟到复查 s<play_seq 因 play_seq
    // 尚未推进而漏判，留下陈旧 READY。先推进后，迟到复查能立即拦住这类写入。
    play_seq_.store(cur + 1, std::memory_order_release);
    const std::uint32_t idx = static_cast<std::uint32_t>(cur % capacity_);
    auto& slot = slots_[idx];
    if (slot.state.load(std::memory_order_acquire) == SlotState::Ready) {
        slot.state.store(SlotState::Empty, std::memory_order_release);
        used_slots_.fetch_sub(1, std::memory_order_relaxed);
    }
    read_offset_ = 0;
    snapshot_current();
}

void JitterBuffer::end_episode() noexcept
{
    episode_dir_ = EpisodeDir::None;
    consecutive_warning_ = 0;
    hold_remaining_ = 0;
    hold_until_target_ = false;
}

// 步长防御：clamp 到 [1, capacity]，避免自定义 step_fn 返回 0 或超大值。
std::uint32_t JitterBuffer::clamp_step(std::uint32_t raw) const noexcept
{
    if (raw == 0) {
        return 1;
    }
    return raw > capacity_ ? capacity_ : raw;
}

// hold 时长（帧）：clamp(step) × F，用 uint64 防溢出后 clamp 到 uint32。
std::uint32_t JitterBuffer::hold_frames(std::uint32_t raw_step) const noexcept
{
    const std::uint32_t step = clamp_step(raw_step);
    const std::uint64_t frames = static_cast<std::uint64_t>(step) * frame_count_;
    return frames > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(frames);
}

JitterBuffer::Action JitterBuffer::decide(std::uint64_t lead, std::uint32_t& skip_step) noexcept
{
    if (episode_dir_ == EpisodeDir::Up) {
        if (lead >= target_slots_) {
            end_episode();
            return Action::None;
        }
        if (hold_until_target_) {
            return Action::Hold;
        }
        if (hold_remaining_ == 0) {
            consecutive_warning_ += 1;
            hold_remaining_ = hold_frames(step_fn_(step_params_, consecutive_warning_));
        }
        return Action::Hold;
    }

    if (episode_dir_ == EpisodeDir::Down) {
        if (lead <= target_slots_) {
            end_episode();
            return Action::None;
        }
        consecutive_warning_ += 1;
        skip_step = clamp_step(step_fn_(step_params_, consecutive_warning_));
        return Action::Skip;
    }

    // 稳态
    if (lead < warning_low_slots_) {
        episode_dir_ = EpisodeDir::Up;
        consecutive_warning_ = 0;
        hold_until_target_ = true;
        return Action::Hold;
    }
    if (lead < normal_low_slots_) {
        episode_dir_ = EpisodeDir::Up;
        consecutive_warning_ = 1;
        hold_remaining_ = hold_frames(step_fn_(step_params_, 1));
        return Action::Hold;
    }
    if (lead <= normal_high_slots_) {
        consecutive_warning_ = 0;
        return Action::None;
    }
    if (lead <= warning_high_slots_) {
        episode_dir_ = EpisodeDir::Down;
        consecutive_warning_ = 1;
        skip_step = clamp_step(step_fn_(step_params_, 1));
        return Action::Skip;
    }
    // deadline 高：一步跳到 60%（步长封顶 N，防御异常跨度）。
    episode_dir_ = EpisodeDir::Down;
    consecutive_warning_ = 0;
    skip_step = static_cast<std::uint32_t>(std::min<std::uint64_t>(lead - target_slots_, capacity_));
    return Action::Skip;
}

JitterBufferPullResult JitterBuffer::pull(std::span<std::byte> output) noexcept
{
    JitterBufferPullResult result {};
    if (output.empty() || frame_bytes_ == 0) {
        return result;
    }
    const std::uint32_t k = static_cast<std::uint32_t>(output.size() / frame_bytes_);
    if (k == 0) {
        return result;
    }

    // 1) startup：lead 达 60% 才建立 anchor。
    if (play_seq_.load(std::memory_order_relaxed) == kNoPlaySeq) {
        const std::uint64_t oldest = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest = highest_seq_.load(std::memory_order_acquire);
        const std::uint64_t lead = (oldest <= highest) ? (highest - oldest + 1) : 0;
        if (lead < target_slots_) {
            std::fill(output.begin(), output.end(), std::byte { 0 });
            result.frames_filled = k;
            result.silence_frames = k;
            return result;
        }
        play_seq_.store(oldest, std::memory_order_release);
        read_offset_ = 0;
        snapshot_current();
        // fall through 到本次即可开始消费
    }

    // 2) 控制决策（每次 pull 一次）。
    const std::uint64_t highest = highest_seq_.load(std::memory_order_acquire);
    const std::uint64_t play = play_seq_.load(std::memory_order_acquire);
    const std::uint64_t lead = (play <= highest) ? (highest - play + 1) : 0;

    std::uint32_t skip_step = 0;
    const Action action = decide(lead, skip_step);

    // 3) Fill（低水位）：全静音，play_seq / read_offset 不动。
    if (action == Action::Hold) {
        std::fill(output.begin(), output.end(), std::byte { 0 });
        if (!hold_until_target_) {
            hold_remaining_ = (hold_remaining_ > k) ? (hold_remaining_ - k) : 0;
        }
        result.frames_filled = k;
        result.silence_frames = k;
        return result;
    }

    // 4) Drop（高水位）：跳过整槽（被跳过的 READY 槽一并回收）。
    if (action == Action::Skip) {
        for (std::uint32_t i = 0; i < skip_step; ++i) {
            advance_slot();
        }
        result.skipped_slots = skip_step;
    }

    // 5) 正常消费（sample 帧游标）。
    std::uint32_t filled = 0;
    std::uint32_t silence = 0;
    while (filled < k) {
        const std::uint64_t p = play_seq_.load(std::memory_order_relaxed);
        if (p > highest) {
            // 耗尽：剩余静音，play_seq 不动。
            std::fill(output.begin() + static_cast<std::ptrdiff_t>(filled) * frame_bytes_,
                output.end(), std::byte { 0 });
            silence += k - filled;
            filled = k;
            break;
        }

        const std::uint32_t idx = static_cast<std::uint32_t>(p % capacity_);
        const std::uint32_t n = std::min(frame_count_ - read_offset_, k - filled);
        if (current_slot_ready_) {
            const std::byte* src = slot_data(idx) + static_cast<std::size_t>(read_offset_) * frame_bytes_;
            std::copy_n(src, static_cast<std::size_t>(n) * frame_bytes_,
                output.data() + static_cast<std::size_t>(filled) * frame_bytes_);
        } else {
            std::fill_n(output.data() + static_cast<std::size_t>(filled) * frame_bytes_,
                static_cast<std::size_t>(n) * frame_bytes_, std::byte { 0 });
            silence += n;
        }
        filled += n;
        read_offset_ += n;
        if (read_offset_ == frame_count_) {
            advance_slot();
        }
    }

    result.frames_filled = filled;
    result.silence_frames = silence;
    return result;
}

void JitterBuffer::reset() noexcept
{
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        slots_[i].state.store(SlotState::Empty, std::memory_order_relaxed);
        slots_[i].sequence = 0;
    }
    play_seq_.store(kNoPlaySeq, std::memory_order_relaxed);
    highest_seq_.store(0, std::memory_order_relaxed);
    oldest_seq_.store(kNoOldestSeq, std::memory_order_relaxed);
    used_slots_.store(0, std::memory_order_relaxed);
    read_offset_ = 0;
    current_slot_ready_ = false;
    episode_dir_ = EpisodeDir::None;
    consecutive_warning_ = 0;
    hold_remaining_ = 0;
    hold_until_target_ = false;
}

} // namespace aqua::audio
