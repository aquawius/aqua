#include "aqua/audio/buffer/jitter_buffer.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

// JitterBuffer 实时路径调试统计开关：默认关闭。
// 这些 log_warn/log_debug 位于 pull()/decide() 实时线程内，spdlog 内部有锁，
// 开启会破坏实时契约；仅在离线排查水位/reanchor 行为时临时置 1。
#ifndef AQUA_JITTER_BUFFER_DEBUG_STAT
#define AQUA_JITTER_BUFFER_DEBUG_STAT 0
#endif

namespace aqua::audio {

namespace {

// play_seq 的"未启动"哨兵；oldest_seq 的"尚无帧"哨兵。
constexpr std::uint64_t kNoPlaySeq = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kNoOldestSeq = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kNoReanchorRequest = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxReanchorJumpFrames = 1'000'000;
constexpr std::uint32_t kReanchorHoldStuckPulls = 5;

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
    // 若显式指定 max_step，则必须覆盖 min_step，否则配置语义自相矛盾。
    return c.step.min_step > 0 && c.step.growth >= 1.0
        && (c.step.max_step == 0 || c.step.max_step >= c.step.min_step);
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
    , reanchor_request_seq_(kNoReanchorRequest)
    , reanchor_count_(0)
    , reanchor_sanity_rejections_(0)
    , last_reanchor_sequence_(kNoReanchorRequest)
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
        log_debug_fmt(
            "JitterBuffer rejected config: slots={} frame_count={} target={:.3f} normal=[{:.3f},{:.3f}] warning=[{:.3f},{:.3f}] step=[{},{}] growth={:.3f}",
            config.capacity_slots, config.frame_count, config.target,
            config.normal_low, config.normal_high, config.warning_low, config.warning_high,
            config.step.min_step, config.step.max_step, config.step.growth);
        return std::unexpected(AudioError::InvalidArgument);
    }
    try {
        auto result = std::unique_ptr<JitterBuffer>(new JitterBuffer(config));
        log_debug_fmt("JitterBuffer created: slots={} frame_count={} frame_bytes={} slot_bytes={} target_slots={} warning=[{},{}] normal=[{},{}] step=[{},{}]",
            result->capacity_, result->frame_count_, result->frame_bytes_, result->slot_bytes_,
            result->target_slots_, result->warning_low_slots_, result->warning_high_slots_,
            result->normal_low_slots_, result->normal_high_slots_,
            result->step_params_.min_step, result->step_params_.max_step);
        return result;
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

    // kNoPlaySeq 保留为「未启动」哨兵，因此 uint64 的最大序列值
    // 在本协议中不是合法的 AudioFrame 序列号。
    if (s == kNoPlaySeq) {
        push_rejected_.fetch_add(1, std::memory_order_relaxed);
        push_rejected_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (frame.frame_count != frame_count_ || frame.data.size() != slot_bytes_) {
        push_rejected_.fetch_add(1, std::memory_order_relaxed);
        push_rejected_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::uint64_t play = play_seq_.load(std::memory_order_acquire);
    const bool started = (play != kNoPlaySeq);
    std::uint64_t highest = highest_seq_.load(std::memory_order_acquire);

    if (started) {
        if (s < play) {
            push_rejected_.fetch_add(1, std::memory_order_relaxed);
            push_rejected_late_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const std::uint64_t distance = s - play;
        if (distance >= capacity_) {
            // 远超前检测有意与「是否接受」分离：producer 上报可能的时间线不连续，
            // 由 consumer 决定何时应用它。
            if (s > highest && (s - highest) > 1) {
                if (distance > kMaxReanchorJumpFrames) {
                    reanchor_sanity_rejections_.fetch_add(1, std::memory_order_relaxed);
                    reanchor_sanity_pending_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_sanity_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                request_reanchor(s);
            }
            // 继续走正常的占用路径；在耗尽场景下，触发帧通常仍落在 EMPTY 槽，
            // 可以无损保留。
        }
    } else {
        const std::uint64_t oldest = oldest_seq_.load(std::memory_order_acquire);
        if (oldest != kNoOldestSeq) {
            if (s >= oldest && s - oldest >= capacity_ && s > highest
                && (s - highest) > 1) {
                if (s - oldest > kMaxReanchorJumpFrames) {
                    reanchor_sanity_rejections_.fetch_add(1, std::memory_order_relaxed);
                    reanchor_sanity_pending_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_sanity_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                // 启动前的远超前数据建立新的候选锚点。这里不清槽；
                // 由 consumer 侧 reanchor 路径在使用新时间线前原子地清掉陈旧 READY 槽，
                // 避免启动期 producer/consumer 的所有权反转。
                oldest_seq_.store(s, std::memory_order_release);
                request_reanchor(s);
            } else if (highest >= s && highest - s >= capacity_) {
                push_rejected_.fetch_add(1, std::memory_order_relaxed);
                push_rejected_late_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }

    const auto idx = static_cast<std::uint32_t>(s % capacity_);
    auto& [state, sequence] = slots_[idx];

    SlotState expected = SlotState::Empty;
    if (!state.compare_exchange_strong(expected, SlotState::Writing,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        push_rejected_.fetch_add(1, std::memory_order_relaxed);
        push_rejected_slot_busy_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    sequence = s;
    std::copy(frame.data.begin(), frame.data.end(), slot_data(idx));

    used_slots_.fetch_add(1, std::memory_order_relaxed);
    state.store(SlotState::Ready, std::memory_order_release);

    const std::uint64_t play2 = play_seq_.load(std::memory_order_acquire);
    if (play2 != kNoPlaySeq && s < play2) {
        SlotState expected_ready = SlotState::Ready;
        if (state.compare_exchange_strong(expected_ready, SlotState::Empty,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            used_slots_.fetch_sub(1, std::memory_order_relaxed);
        }
        push_rejected_.fetch_add(1, std::memory_order_relaxed);
        push_rejected_late_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint64_t cur = highest_seq_.load(std::memory_order_relaxed);
    if (s > cur) {
        highest_seq_.store(s, std::memory_order_release);
    }
    cur = oldest_seq_.load(std::memory_order_relaxed);
    if (cur == kNoOldestSeq || s < cur) {
        oldest_seq_.store(s, std::memory_order_release);
    }
    push_accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void JitterBuffer::request_reanchor(std::uint64_t sequence) noexcept
{
    reanchor_requests_.fetch_add(1, std::memory_order_relaxed);
    auto current = reanchor_request_seq_.load(std::memory_order_relaxed);
    while (current == kNoReanchorRequest || sequence > current) {
        if (reanchor_request_seq_.compare_exchange_weak(current, sequence,
                std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }
    }
}


void JitterBuffer::apply_reanchor(std::uint64_t sequence) noexcept
{
    // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
    const auto old_play = play_seq_.load(std::memory_order_relaxed);
    const auto old_highest = highest_seq_.load(std::memory_order_relaxed);
    const auto old_used = used_slots_.load(std::memory_order_relaxed);
    std::uint32_t removed_ready = 0;
#endif

    // 保留已落在新接收窗口内的 READY 帧，丢弃陈旧帧。
    // WRITING 槽不处理；若其 sequence 已落后于新播放时间线，
    // producer 侧的迟到复查会回收它们。
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        auto& slot = slots_[i];
        if (slot.state.load(std::memory_order_acquire) != SlotState::Ready) {
            continue;
        }
        const auto q = slot.sequence;
        const bool in_window = q >= sequence && (q - sequence) < capacity_;
        if (!in_window) {
            SlotState expected = SlotState::Ready;
            if (slot.state.compare_exchange_strong(expected, SlotState::Empty,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                used_slots_.fetch_sub(1, std::memory_order_relaxed);

                // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
                ++removed_ready;
#endif
            }
        }
    }

    play_seq_.store(sequence, std::memory_order_release);
    read_offset_ = 0;
    current_slot_ready_ = false;
    end_episode();
    episode_dir_ = EpisodeDir::Up;
    fill_episodes_.fetch_add(1, std::memory_order_relaxed);
    hold_until_target_ = true;
    last_hold_lead_ = 0;
    hold_stuck_pulls_ = 0;
    reanchor_count_.fetch_add(1, std::memory_order_relaxed);
    last_reanchor_sequence_.store(sequence, std::memory_order_release);
    snapshot_current();

    // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
    const auto new_highest = highest_seq_.load(std::memory_order_relaxed);
    const auto new_used = used_slots_.load(std::memory_order_relaxed);
    log_warn_fmt(
        "JitterBuffer water adjustment: REANCHOR play {}->{} highest {}->{} used {}->{} removed_ready={} hold_until_target=1",
        old_play == kNoPlaySeq ? 0 : old_play, sequence,
        old_highest, new_highest, old_used, new_used, removed_ready);
#endif
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
    SlotState expected_ready = SlotState::Ready;
    if (slot.state.compare_exchange_strong(expected_ready, SlotState::Empty,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
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
            // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
            log_warn_fmt(
                "JitterBuffer water adjustment: FILL complete lead={}/{} target={} episode_steps={}",
                lead, capacity_, target_slots_, consecutive_warning_);
#endif
            end_episode();
            return Action::None;
        }
        if (hold_until_target_) {
            return Action::Hold;
        }
        if (hold_remaining_ == 0) {
            consecutive_warning_ += 1;
            const auto step = clamp_step(step_fn_(step_params_, consecutive_warning_));
            hold_remaining_ = hold_frames(step);
            // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
            log_warn_fmt(
                "JitterBuffer water adjustment: FILL continue lead={}/{} target={} step={} hold_frames={} episode_step={}",
                lead, capacity_, target_slots_, step, hold_remaining_, consecutive_warning_);
#endif
        }
        return Action::Hold;
    }

    if (episode_dir_ == EpisodeDir::Down) {
        if (lead <= target_slots_) {
            // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
            log_warn_fmt(
                "JitterBuffer water adjustment: DROP complete lead={}/{} target={} episode_steps={}",
                lead, capacity_, target_slots_, consecutive_warning_);
#endif
            end_episode();
            return Action::None;
        }
        consecutive_warning_ += 1;
        skip_step = clamp_step(step_fn_(step_params_, consecutive_warning_));
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP lead={}/{} target={} step={} warning={}",
            lead, capacity_, target_slots_, skip_step, consecutive_warning_);
#endif
        return Action::Skip;
    }

    // 稳态
    if (lead < warning_low_slots_) {
        episode_dir_ = EpisodeDir::Up;
        fill_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 0;
        hold_until_target_ = true;
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: FILL enter lead={}/{} warning_low={} target={} mode=hold_until_target",
            lead, capacity_, warning_low_slots_, target_slots_);
#endif
        return Action::Hold;
    }
    if (lead < normal_low_slots_) {
        episode_dir_ = EpisodeDir::Up;
        fill_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 1;
        hold_remaining_ = hold_frames(step_fn_(step_params_, 1));
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: FILL enter lead={}/{} normal_low={} target={} step={} hold_frames={}",
            lead, capacity_, normal_low_slots_, target_slots_, consecutive_warning_, hold_remaining_);
#endif
        return Action::Hold;
    }
    if (lead <= normal_high_slots_) {
        consecutive_warning_ = 0;
        return Action::None;
    }
    if (lead <= warning_high_slots_) {
        episode_dir_ = EpisodeDir::Down;
        drop_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 1;
        skip_step = clamp_step(step_fn_(step_params_, 1));
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP enter lead={}/{} normal_high={} warning_high={} target={} step={}",
            lead, capacity_, normal_high_slots_, warning_high_slots_, target_slots_, skip_step);
#endif
        return Action::Skip;
    }
    // deadline 高：一步跳到 60%（步长封顶 N，防御异常跨度）。
    episode_dir_ = EpisodeDir::Down;
    drop_episodes_.fetch_add(1, std::memory_order_relaxed);
    consecutive_warning_ = 0;
    skip_step = static_cast<std::uint32_t>(std::min<std::uint64_t>(lead - target_slots_, capacity_));
    // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
    log_warn_fmt(
        "JitterBuffer water adjustment: DROP deadline-high lead={}/{} target={} step={}",
        lead, capacity_, target_slots_, skip_step);
#endif
    return Action::Skip;
}

JitterBufferPullResult JitterBuffer::pull(std::span<std::byte> output) noexcept
{
    JitterBufferPullResult result {};
    pull_calls_.fetch_add(1, std::memory_order_relaxed);
    if (output.empty() || frame_bytes_ == 0 || (output.size() % frame_bytes_) != 0) {
        return result;
    }
    const std::uint32_t k = static_cast<std::uint32_t>(output.size() / frame_bytes_);
    if (k == 0) {
        return result;
    }
    // 每次 pull 最多消费一个最新的 reanchor 请求。延迟的请求保存在 consumer 侧私有状态，
    // 直到应用它既安全又有意义。
    const auto request = reanchor_request_seq_.exchange(kNoReanchorRequest,
        std::memory_order_acq_rel);
    if (request != kNoReanchorRequest) {
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        const auto previous_deferred = deferred_reanchor_seq_;
#endif

        if (deferred_reanchor_seq_ == kNoReanchorRequest) {
            deferred_reanchor_seq_ = request;
        } else {
            deferred_reanchor_seq_ = std::max(deferred_reanchor_seq_, request);
        }
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: reanchor request received={} deferred={} previous_deferred={}",
            request, deferred_reanchor_seq_,
            previous_deferred == kNoReanchorRequest ? 0 : previous_deferred);
#endif
        last_hold_lead_ = 0;
        hold_stuck_pulls_ = 0;
    }

    auto highest = highest_seq_.load(std::memory_order_acquire);
    auto play = play_seq_.load(std::memory_order_acquire);

    if (deferred_reanchor_seq_ != kNoReanchorRequest) {
        // 一旦播放已越过其锚点，请求即作废。
        // 否则，当当前时间线已耗尽、或已跨越至少一个完整接收窗口时立即应用。
        // 后者是「远超前帧被保留在 JB 中」的重要快路径：发布后 highest_seq_ 已指向
        // 那个远端，若还等 play_seq >= highest_seq_ 就会被迫做 O(gap/N) 次人为跳过。
        if (play != kNoPlaySeq && play >= deferred_reanchor_seq_) {
            deferred_reanchor_seq_ = kNoReanchorRequest;
            reanchor_cancels_.fetch_add(1, std::memory_order_relaxed);
            last_hold_lead_ = 0;
            hold_stuck_pulls_ = 0;
        } else if (play != kNoPlaySeq) {
            const auto lead_now = (play <= highest) ? (highest - play + 1) : 0;
            if (play >= highest || lead_now >= capacity_) {
                const auto r = deferred_reanchor_seq_;
                deferred_reanchor_seq_ = kNoReanchorRequest;
                apply_reanchor(r);
                highest = highest_seq_.load(std::memory_order_acquire);
                play = play_seq_.load(std::memory_order_acquire);
            }
        }
    }

    // 启动与恢复使用相同的「填到 target」策略，只是此时还没有播放锚点。
    // 启动快照会二次校验，避免并发的启动前 rebase 静默锚定到已过期的窗口。
    if (play_seq_.load(std::memory_order_acquire) == kNoPlaySeq
        && deferred_reanchor_seq_ != kNoReanchorRequest) {
        const auto r = deferred_reanchor_seq_;
        deferred_reanchor_seq_ = kNoReanchorRequest;
        apply_reanchor(r);
        highest = highest_seq_.load(std::memory_order_acquire);
        play = play_seq_.load(std::memory_order_acquire);
    }

    if (play_seq_.load(std::memory_order_relaxed) == kNoPlaySeq) {
        const std::uint64_t oldest1 = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest1 = highest_seq_.load(std::memory_order_acquire);
        const std::uint64_t lead1 = (oldest1 <= highest1) ? (highest1 - oldest1 + 1) : 0;
        if (lead1 < target_slots_) {
            std::fill(output.begin(), output.end(), std::byte { 0 });
            result.frames_filled = k;
            result.silence_frames = k;
            return result;
        }

        const std::uint64_t oldest2 = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest2 = highest_seq_.load(std::memory_order_acquire);
        if (oldest1 != oldest2 || highest1 != highest2) {
            std::fill(output.begin(), output.end(), std::byte { 0 });
            result.frames_filled = k;
            result.silence_frames = k;
            return result;
        }

        play_seq_.store(oldest2, std::memory_order_release);
        read_offset_ = 0;
        snapshot_current();
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_debug_fmt(
            "JitterBuffer timeline anchor established: play_seq={} lead={}/{} target={} used_slots={}",
            oldest2, lead1, capacity_, target_slots_, used_slots_.load(std::memory_order_relaxed));
#endif
        play = oldest2;
        highest = highest2;
    }

    highest = highest_seq_.load(std::memory_order_acquire);
    play = play_seq_.load(std::memory_order_acquire);
    const std::uint64_t lead = (play <= highest) ? (highest - play + 1) : 0;

    std::uint32_t skip_step = 0;
    Action action = decide(lead, skip_step);

    if (deferred_reanchor_seq_ != kNoReanchorRequest && action == Action::Hold) {
        if (hold_stuck_pulls_ == 0) {
            last_hold_lead_ = lead;
            hold_stuck_pulls_ = 1;
        } else if (lead <= last_hold_lead_) {
            ++hold_stuck_pulls_;
        } else {
            hold_stuck_pulls_ = 1;
        }
        last_hold_lead_ = lead;

        if (hold_stuck_pulls_ >= kReanchorHoldStuckPulls) {
            const auto r = deferred_reanchor_seq_;
            deferred_reanchor_seq_ = kNoReanchorRequest;
            apply_reanchor(r);
            highest = highest_seq_.load(std::memory_order_acquire);
            play = play_seq_.load(std::memory_order_acquire);
            const auto new_lead = (play <= highest) ? (highest - play + 1) : 0;
            skip_step = 0;
            action = decide(new_lead, skip_step);
        }
    } else {
        last_hold_lead_ = 0;
        hold_stuck_pulls_ = 0;
    }

    if (action == Action::Hold) {
        std::fill(output.begin(), output.end(), std::byte { 0 });
        if (!hold_until_target_) {
            hold_remaining_ = (hold_remaining_ > k) ? (hold_remaining_ - k) : 0;
        }
        result.frames_filled = k;
        result.silence_frames = k;
        fill_hold_frames_.fetch_add(k, std::memory_order_relaxed);
        pull_frames_.fetch_add(k, std::memory_order_relaxed);
        pull_silence_frames_.fetch_add(k, std::memory_order_relaxed);
        return result;
    }

    if (action == Action::Skip) {
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        const auto before_skip = play_seq_.load(std::memory_order_relaxed);
#endif
        for (std::uint32_t i = 0; i < skip_step; ++i) {
            advance_slot();
        }
        result.skipped_slots = skip_step;
        drop_skipped_slots_.fetch_add(skip_step, std::memory_order_relaxed);
        // for debug jitter buffer stat.
#if AQUA_JITTER_BUFFER_DEBUG_STAT
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP applied play_seq {}->{} skipped_slots={} used_slots={} lead_before={}",
            before_skip, play_seq_.load(std::memory_order_relaxed), skip_step,
            used_slots_.load(std::memory_order_relaxed), lead);
#endif
    }

    std::uint32_t filled = 0;
    std::uint32_t silence = 0;
    while (filled < k) {
        const std::uint64_t p = play_seq_.load(std::memory_order_relaxed);
        if (p > highest) {
            std::fill(output.begin() + static_cast<std::ptrdiff_t>(filled) * frame_bytes_,
                output.end(), std::byte { 0 });
            silence += k - filled;
            filled = k;
            break;
        }

        const std::uint32_t idx = static_cast<std::uint32_t>(p % capacity_);
        const std::uint32_t n = std::min(frame_count_ - read_offset_, k - filled);
        if (current_slot_ready_) {
            const std::byte* src = slot_data(idx)
                + static_cast<std::size_t>(read_offset_) * frame_bytes_;
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
    pull_frames_.fetch_add(filled, std::memory_order_relaxed);
    pull_silence_frames_.fetch_add(silence, std::memory_order_relaxed);
    return result;
}


void JitterBuffer::reset() noexcept
{
    log_debug_fmt("JitterBuffer reset: capacity_slots={} frame_count={} frame_bytes={}",
        capacity_, frame_count_, frame_bytes_);
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        slots_[i].state.store(SlotState::Empty, std::memory_order_relaxed);
        slots_[i].sequence = 0;
    }
    play_seq_.store(kNoPlaySeq, std::memory_order_relaxed);
    highest_seq_.store(0, std::memory_order_relaxed);
    oldest_seq_.store(kNoOldestSeq, std::memory_order_relaxed);
    used_slots_.store(0, std::memory_order_relaxed);
    reanchor_request_seq_.store(kNoReanchorRequest, std::memory_order_relaxed);
    push_accepted_.store(0, std::memory_order_relaxed);
    push_rejected_.store(0, std::memory_order_relaxed);
    push_rejected_late_.store(0, std::memory_order_relaxed);
    push_rejected_slot_busy_.store(0, std::memory_order_relaxed);
    push_rejected_invalid_.store(0, std::memory_order_relaxed);
    push_rejected_sanity_.store(0, std::memory_order_relaxed);
    pull_calls_.store(0, std::memory_order_relaxed);
    pull_frames_.store(0, std::memory_order_relaxed);
    pull_silence_frames_.store(0, std::memory_order_relaxed);
    fill_episodes_.store(0, std::memory_order_relaxed);
    fill_hold_frames_.store(0, std::memory_order_relaxed);
    drop_episodes_.store(0, std::memory_order_relaxed);
    drop_skipped_slots_.store(0, std::memory_order_relaxed);
    reanchor_requests_.store(0, std::memory_order_relaxed);
    reanchor_cancels_.store(0, std::memory_order_relaxed);
    reanchor_count_.store(0, std::memory_order_relaxed);
    reanchor_sanity_rejections_.store(0, std::memory_order_relaxed);
    reanchor_sanity_pending_.store(0, std::memory_order_relaxed);
    last_reanchor_sequence_.store(kNoReanchorRequest, std::memory_order_relaxed);
    read_offset_ = 0;
    current_slot_ready_ = false;
    episode_dir_ = EpisodeDir::None;
    consecutive_warning_ = 0;
    hold_remaining_ = 0;
    hold_until_target_ = false;
    deferred_reanchor_seq_ = kNoReanchorRequest;
    last_hold_lead_ = 0;
    hold_stuck_pulls_ = 0;
}

} // namespace aqua::audio
