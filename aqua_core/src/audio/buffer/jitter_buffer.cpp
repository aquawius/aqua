#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/buffer/buffer_config.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

// Phase 2 concealment 淡出增益用 Q15 定点：32768 = 1.0。RT 路径里避免对
// 每个样本做 double 乘法（每 slot 只换算一次增益）。
constexpr std::uint32_t kGainOneQ15 = 32768;
// JitterBuffer 实时路径调试统计开关：默认关闭。
// 这些 log_warn/log_debug 位于 pull()/decide() 实时线程内，spdlog 内部有锁，
// 开启会破坏实时契约；仅在离线排查水位/reanchor 行为时临时置 1。
#ifndef AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
#define AQUA_JB_RUNTIME_THREAD_DEBUG_LOG 0
#endif
// JitterBuffer 控制面（producer / push strand）判定日志开关：默认关闭。
// push() 运行在网络 strand 上，不是 RT；这里的日志回答"为什么 reanchor /
// 为什么没 reanchor"。RT 侧日志（pull/decide/reanchor 应用）归上面那个宏。
// 点位全表见 aqua_core/doc/modules/observability.md。
#ifndef AQUA_JB_CONTROL_THREAD_DEBUG_LOG
#define AQUA_JB_CONTROL_THREAD_DEBUG_LOG 0
#endif

namespace aqua::audio {

namespace {

    // play_seq 的"未启动"哨兵；oldest_seq 的"尚无帧"哨兵。
    constexpr std::uint64_t kNoPlaySeq = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t kNoOldestSeq = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t kNoReanchorRequest = std::numeric_limits<std::uint64_t>::max();

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

    // Warning 区保持温和：连续 config::JB_WARNING_GROWTH_INTERVAL 次 warning
    // 评估才允许步长按 growth 增长一级。默认参数因此得到：1,1,1,1,2,2,2,2,3...
    // （30-slot 时上限通常为 3）。
    const std::uint32_t growth_levels = k == 0 ? 0u
                                               : (k - 1u) / config::JB_WARNING_GROWTH_INTERVAL;

    double step = static_cast<double>(base);
    for (std::uint32_t i = 0; i < growth_levels && step < static_cast<double>(cap); ++i) {
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
        if (c.capacity_slots < config::JB_MIN_CAPACITY_SLOTS || c.frame_count == 0) {
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
        // 最小容量为 4 个 slot，避免整数水位量化后 warning 区间塌缩为 0 slot。
        // 阈值严格有序且落在 (0,1]。startup_level 独立于稳态阈值序，仅要求
        // 落在 (0,1]（pre-roll 水位可自由调节，不受稳态区间约束）。
        if (!(c.warning_low > 0.0
                && c.warning_low < c.normal_low
                && c.normal_low < c.target
                && c.target < c.normal_high
                && c.normal_high < c.warning_high
                && c.warning_high <= 1.0)) {
            return false;
        }
        if (!(c.startup_level > 0.0 && c.startup_level <= 1.0)) {
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
    , silence_byte_(config.format.silence_byte())
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
            config::JB_WARNING_STEP_AUTO_MIN_SLOTS,
            round_pct(config::JB_WARNING_STEP_AUTO_FRACTION, capacity_));
        step_params_.max_step = auto_max;
    }
    startup_slots_ = std::max<std::uint32_t>(1, round_pct(config.startup_level, capacity_));
    // Phase 2 concealment 几何（构造期定形；pull 内不再分配、不再读 config）。
    // max_slots 即使关闭也保留：late 包 "本可用" 的测量窗口就是它（细则 §14）。
    format_ = config.format;
    bytes_per_sample_ = config.format.bytes_per_sample();
    conceal_max_slots_ = config.concealment.max_slots;
    conceal_enabled_ = config.concealment.enabled && conceal_max_slots_ > 0;
    if (conceal_enabled_) {
        // 消费侧私有的"上一个真实 slot"副本。必须拷贝而非持 ring slot 指针：
        // advance_slot() 会把槽 CAS 回 Empty，producer 随后可覆写，持指针就
        // 变成依赖另一个 RT producer 的共享可变状态（违反 RT 契约）。
        // 分配失败由 create() 的 bad_alloc 捕获转 BackendFailed。
        last_pcm_.resize(slot_bytes_);
    }
    target_slots_ = std::max<std::uint32_t>(1, round_pct(config.target, capacity_));
    // band 相对 target 的倍率（create 时按初始 target 快照；此后恒定，固定
    // 模式下恒等于下面四个初始值）。
    const double target_base = static_cast<double>(target_slots_.load(std::memory_order_relaxed));
    band_wl_per_target_ = static_cast<double>(round_pct(config.warning_low, capacity_)) / target_base;
    band_nl_per_target_ = static_cast<double>(round_pct(config.normal_low, capacity_)) / target_base;
    band_nh_per_target_ = static_cast<double>(round_pct(config.normal_high, capacity_)) / target_base;
    band_wh_per_target_ = static_cast<double>(round_pct(config.warning_high, capacity_)) / target_base;
    // 预计算整张带表（target 0..capacity_），此后只读：RT 侧一次查表即拿到与
    // 本拍 target 同源的四个带值，pull 路径既无浮点也无第二次原子读。
    band_table_.resize(static_cast<std::size_t>(capacity_ + 1) * BandCount);
    for (std::uint32_t t = 0; t <= capacity_; ++t) {
        const double n = static_cast<double>(t);
        band_table_[static_cast<std::size_t>(t) * BandCount + BandWarningLow] = static_cast<std::uint32_t>(std::lround(n * band_wl_per_target_));
        band_table_[static_cast<std::size_t>(t) * BandCount + BandNormalLow] = static_cast<std::uint32_t>(std::lround(n * band_nl_per_target_));
        band_table_[static_cast<std::size_t>(t) * BandCount + BandNormalHigh] = static_cast<std::uint32_t>(std::lround(n * band_nh_per_target_));
        band_table_[static_cast<std::size_t>(t) * BandCount + BandWarningHigh] = static_cast<std::uint32_t>(std::lround(n * band_wh_per_target_));
    }
}

JitterBuffer::~JitterBuffer() = default;

std::expected<std::unique_ptr<JitterBuffer>, AudioError>
JitterBuffer::create(const JitterBufferConfig& config)
{
    if (!config_is_valid(config)) {
        log_debug_fmt(
            "JitterBuffer rejected config: slots={} frame_count={} target={:.3f} normal=[{:.3f},{:.3f}] warning=[{:.3f},{:.3f}] startup={:.3f} step=[{},{}] growth={:.3f}",
            config.capacity_slots, config.frame_count, config.target,
            config.normal_low, config.normal_high, config.warning_low, config.warning_high,
            config.startup_level,
            config.step.min_step, config.step.max_step, config.step.growth);
        return std::unexpected(AudioError::InvalidArgument);
    }
    try {
        auto result = std::unique_ptr<JitterBuffer>(new JitterBuffer(config));
        log_debug_fmt("JitterBuffer created: slots={} frame_count={} frame_bytes={} slot_bytes={} startup_slots={} target_slots={} warning=[{},{}] normal=[{},{}] step=[{},{}]",
            result->capacity_, result->frame_count_, result->frame_bytes_, result->slot_bytes_,
            result->startup_slots_,
            result->target_slots(), result->warning_low_slots(), result->warning_high_slots(),
            result->normal_low_slots(), result->normal_high_slots(),
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
    const auto lead = lead_slots();
    // lead 可能超过 capacity（reanchor 请求待应用时，超窗的帧会被受理并抬高
    // highest）。水位是诊断量，裁剪到 [0,1] 便于上层按百分比解释。
    const auto level = static_cast<double>(lead) / static_cast<double>(capacity_);
    return level > 1.0 ? 1.0 : level;
}

std::uint32_t JitterBuffer::lead_slots() const noexcept
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
    // 与 water_level 的历史口径一致：裁剪到容量，避免超窗时给出 > capacity
    // 的"伪 lead"（reanchor 待应用期间 highest 会被抬高）。
    return lead > capacity_ ? capacity_ : static_cast<std::uint32_t>(lead);
}

std::uint64_t JitterBuffer::play_sequence() const noexcept
{
    const auto play = play_seq_.load(std::memory_order_acquire);
    // 未锚定对外表示为 0（kNoPlaySeq 是内部哨兵，暴露会污染诊断显示）。
    return play == kNoPlaySeq ? 0 : play;
}

std::uint64_t JitterBuffer::highest_received_sequence() const noexcept
{
    return highest_seq_.load(std::memory_order_acquire);
}

void JitterBuffer::set_target_slots(std::uint32_t slots) noexcept
{
    if (slots < 1) {
        slots = 1;
    }
    if (slots > capacity_) {
        slots = capacity_;
    }
    // 只写 target 一个原子。四个带值由构造期预计算的 band_table_ 按 target
    // 现算（RT 侧一次查表），因此不存在「新 target + 旧 band」的撕裂窗口：
    // 旧实现把 target 与四个带分五次 store，decide() 分五次读，可能在一拍内
    // 用错配的阈值判成 Fill/Drop。
    target_slots_.store(slots, std::memory_order_relaxed);
}

JitterBuffer::Bands JitterBuffer::bands() const noexcept
{
    return bands_from(target_slots());
}

bool JitterBuffer::reanchor_pending() const noexcept
{
    return reanchor_request_seq_.load(std::memory_order_acquire) != kNoReanchorRequest
        || deferred_reanchor_seq_.load(std::memory_order_acquire) != kNoReanchorRequest;
}

std::uint64_t JitterBuffer::reanchor_target_sequence() const noexcept
{
    const auto request = reanchor_request_seq_.load(std::memory_order_acquire);
    const auto deferred = deferred_reanchor_seq_.load(std::memory_order_acquire);
    std::uint64_t target = 0;
    if (request != kNoReanchorRequest) {
        target = request;
    }
    if (deferred != kNoReanchorRequest && deferred > target) {
        target = deferred;
    }
    return target;
}

void JitterBuffer::record_silence_run(std::uint32_t silence_frames) noexcept
{
    // 每 pull 一次的 gauge 发布点：record_silence_run() 是 pull() 全部路径
    // （pre-roll / Hold / Skip / 主循环）的唯一汇聚点，因此在这里把 consumer
    // 私有的 conceal/underrun run 镜像给诊断线程——跨线程读只读镜像，RT 侧
    // 的计数器保持普通成员，热路径零改动。
    conceal_run_slots_out_.store(conceal_run_slots_, std::memory_order_relaxed);
    underrun_run_slots_out_.store(underrun_run_slots_, std::memory_order_relaxed);
    if (silence_frames == 0) {
        // 出现真实数据：当前连续断流 run 结束。
        consecutive_silence_frames_.store(0, std::memory_order_relaxed);
        return;
    }
    const auto run = consecutive_silence_frames_.fetch_add(
                         silence_frames, std::memory_order_relaxed)
        + silence_frames;
    auto observed = max_silence_run_frames_.load(std::memory_order_relaxed);
    while (run > observed
        && !max_silence_run_frames_.compare_exchange_weak(observed, run,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

// ---- Phase 2 concealment（全部在 consumer RT 线程内执行）----

void JitterBuffer::scale_frames(std::byte* dst, std::uint32_t frames, std::uint32_t gain_q15) const noexcept
{
    // 1.0 是首个掩盖包的常态：整包直接重复，免掉逐样本缩放。
    if (gain_q15 >= kGainOneQ15 || dst == nullptr || frames == 0) {
        return;
    }
    const auto gain = static_cast<std::int64_t>(gain_q15);
    const std::uint32_t count = frames * format_.channels;
    std::byte* p = dst;
    // 编码分派在循环外：循环内无 switch、无函数调用、无分配。
    switch (format_.encoding) {
    case AudioEncoding::PCM_F32LE: {
        const auto g = static_cast<float>(gain_q15) / static_cast<float>(kGainOneQ15);
        for (std::uint32_t i = 0; i < count; ++i, p += bytes_per_sample_) {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof v); // 输出缓冲不对齐假设：统一走 memcpy
            v *= g;
            std::memcpy(p, &v, sizeof v);
        }
        break;
    }
    case AudioEncoding::PCM_S16LE: {
        for (std::uint32_t i = 0; i < count; ++i, p += bytes_per_sample_) {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof v);
            const auto scaled = (static_cast<std::int64_t>(v) * gain + (kGainOneQ15 / 2)) >> 15;
            const auto out = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(scaled, -32768, 32767));
            std::memcpy(p, &out, sizeof out);
        }
        break;
    }
    case AudioEncoding::PCM_S32LE: {
        for (std::uint32_t i = 0; i < count; ++i, p += bytes_per_sample_) {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof v);
            const auto scaled = (static_cast<std::int64_t>(v) * gain + (kGainOneQ15 / 2)) >> 15;
            const auto out = static_cast<std::int32_t>(
                std::clamp<std::int64_t>(scaled, INT32_MIN, INT32_MAX));
            std::memcpy(p, &out, sizeof out);
        }
        break;
    }
    case AudioEncoding::PCM_S24LE: {
        for (std::uint32_t i = 0; i < count; ++i, p += bytes_per_sample_) {
            const auto b0 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0]));
            const auto b1 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1]));
            const auto b2 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2]));
            std::uint32_t raw = b0 | (b1 << 8) | (b2 << 16);
            const std::int32_t v = (raw & 0x800000u) != 0
                ? static_cast<std::int32_t>(raw) - 0x1000000
                : static_cast<std::int32_t>(raw);
            const auto scaled = (static_cast<std::int64_t>(v) * gain + (kGainOneQ15 / 2)) >> 15;
            const auto out = static_cast<std::int32_t>(
                std::clamp<std::int64_t>(scaled, -8388608, 8388607));
            const auto u = static_cast<std::uint32_t>(out);
            p[0] = static_cast<std::byte>(u & 0xFFu);
            p[1] = static_cast<std::byte>((u >> 8) & 0xFFu);
            p[2] = static_cast<std::byte>((u >> 16) & 0xFFu);
        }
        break;
    }
    case AudioEncoding::PCM_U8: {
        // U8 静音是 0x80，缩放要绕偏置做（silence_byte_ 即偏置本身）。
        const auto bias = std::to_integer<std::int32_t>(silence_byte_);
        for (std::uint32_t i = 0; i < count; ++i, p += bytes_per_sample_) {
            const auto centered = std::to_integer<std::int32_t>(p[0]) - bias;
            const auto scaled = (static_cast<std::int64_t>(centered) * gain + (kGainOneQ15 / 2)) >> 15;
            const auto out = std::clamp<std::int64_t>(scaled, -bias, 255 - bias);
            p[0] = static_cast<std::byte>(static_cast<std::uint8_t>(out + bias));
        }
        break;
    }
    case AudioEncoding::INVALID:
        break;
    }
}

std::uint32_t JitterBuffer::conceal_gain_for(std::uint32_t run_index) const noexcept
{
    if (run_index >= conceal_max_slots_) {
        return 0;
    }
    // 线性淡出：第 i 个掩盖包增益 = (max - i) / max（3 包 → 1.0 / 0.67 / 0.33）。
    const double ratio = static_cast<double>(conceal_max_slots_ - run_index)
        / static_cast<double>(conceal_max_slots_);
    const auto q = static_cast<std::uint32_t>(ratio * static_cast<double>(kGainOneQ15) + 0.5);
    return q > kGainOneQ15 ? kGainOneQ15 : q;
}

void JitterBuffer::mark_underrun() noexcept
{
    if (!underrun_active_) {
        underrun_active_ = true;
        underrun_events_.fetch_add(1, std::memory_order_relaxed);
        // for debug jitter buffer stat（RT 侧埋点：进入"无真实 PCM"的那个瞬间）。
        // 之前只有计数器，事后无从判断这次欠载发生在什么水位/target/掩盖状态下。
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        const auto play = play_seq_.load(std::memory_order_relaxed);
        const auto highest = highest_seq_.load(std::memory_order_relaxed);
        log_warn_fmt(
            "JitterBuffer underrun enter (no real PCM): play_seq={} highest={} lead={} target={} used_slots={} capacity={} conceal_enabled={} conceal_run_slots={} underrun_run_slots={} events={}",
            play, highest, lead_slots(), target_slots(),
            used_slots_.load(std::memory_order_relaxed), capacity_,
            conceal_enabled_ ? 1 : 0, conceal_run_slots_, underrun_run_slots_,
            underrun_events_.load(std::memory_order_relaxed));
#endif
    }
}

void JitterBuffer::on_slot_boundary(bool ready) noexcept
{
    if (ready) {
        // for debug jitter buffer stat（RT 侧埋点：掩盖/欠载 episode 结束）。
        // 边沿触发：underrun_run_slots_ 马上归零，故整个 episode 只打一行。
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        if (underrun_run_slots_ != 0) {
            log_warn_fmt(
                "JitterBuffer conceal exit (real PCM back): play_seq={} run_slots={} concealed_slots={} saturated_slots={} underrun_events={} max_underrun_run={}",
                play_seq_.load(std::memory_order_relaxed), underrun_run_slots_,
                concealed_slots_.load(std::memory_order_relaxed),
                concealed_saturated_slots_.load(std::memory_order_relaxed),
                underrun_events_.load(std::memory_order_relaxed),
                max_consecutive_underrun_slots_.load(std::memory_order_relaxed));
        }
#endif
        // 真实数据：连续缺帧 / 连续掩盖 run 都结束。
        conceal_run_slots_ = 0;
        underrun_run_slots_ = 0;
        underrun_active_ = false;
        conceal_active_ = false;
        return;
    }
    // 缺帧 slot：掩盖与否都算 underrun（"播放头推进到没有真实 PCM 的 slot"）。
    mark_underrun();
    ++underrun_run_slots_;
    if (underrun_run_slots_ > max_consecutive_underrun_slots_.load(std::memory_order_relaxed)) {
        max_consecutive_underrun_slots_.store(underrun_run_slots_, std::memory_order_relaxed);
    }
    if (!conceal_enabled_ || !have_last_pcm_) {
        // 关闭、或还没有任何真实 PCM 可重复（首个 slot 就缺）→ 静音。
        conceal_active_ = false;
        return;
    }
    if (conceal_run_slots_ >= conceal_max_slots_) {
        // for debug jitter buffer stat（RT 侧埋点：饱和转静音的跃迁，一行一次）。
        // 条件用 conceal_active_ 做边沿：置 false 之后本分支不再重复打印。
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        if (conceal_active_) {
            log_warn_fmt(
                "JitterBuffer conceal saturated -> silence: play_seq={} run_slots={} max_slots={} saturated_slots={} underrun_run_slots={}",
                play_seq_.load(std::memory_order_relaxed), conceal_run_slots_,
                conceal_max_slots_,
                concealed_saturated_slots_.load(std::memory_order_relaxed),
                underrun_run_slots_);
        }
#endif
        // 封顶（细则 §9）：超过最大连续掩盖长度后进入静音。
        concealed_saturated_slots_.fetch_add(1, std::memory_order_relaxed);
        conceal_active_ = false;
        return;
    }
    conceal_gain_q15_ = conceal_gain_for(conceal_run_slots_);
    ++conceal_run_slots_;
    concealed_slots_.fetch_add(1, std::memory_order_relaxed);
    conceal_active_ = true;
    // for debug jitter buffer stat（RT 侧埋点：进入掩盖，带"第几包"）。
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
    if (conceal_run_slots_ == 1) {
        log_warn_fmt(
            "JitterBuffer conceal enter (repeat last PCM): play_seq={} gain_q15={} max_slots={}",
            play_seq_.load(std::memory_order_relaxed), conceal_gain_q15_, conceal_max_slots_);
    }
#endif
}

void JitterBuffer::note_late_packet(std::uint64_t lateness_slots) noexcept
{
    // 细则 §14：late 包本阶段继续 drop，只记录"本可用"潜力——落后播放头
    // 不超过 conceal 窗口，说明它到达时对应 slot 还在被掩盖/静音，插入即用。
    if (conceal_max_slots_ == 0 || lateness_slots > conceal_max_slots_) {
        return;
    }
    late_useful_packets_.fetch_add(1, std::memory_order_relaxed);
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
            note_late_packet(play - s);
            push_rejected_.fetch_add(1, std::memory_order_relaxed);
            push_rejected_late_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const std::uint64_t distance = s - play;
        if (distance >= capacity_) {
            // 远超前检测有意与「是否接受」分离：producer 上报可能的时间线不连续，
            // 由 consumer 决定何时应用它。缺口必须达到 config::JB_REANCHOR_MIN_GAP_SLOTS
            // 才视为断裂；否则只是 ring 满后的顺序溢出（highest 冻结、s 逐包递增），
            // 交给 consumer 的 deadline-high DROP 兜底，避免误触发 reanchor 风暴。
            const bool gap_reaches_break = s > highest
                && (s - highest) >= config::JB_REANCHOR_MIN_GAP_SLOTS;
            // 控制面日志（#8，见本文件顶部说明）：把"为什么 reanchor / 为什么
            // 没 reanchor"打全——缺口、MIN_GAP 比较、容量三者缺一不可解释。
            // 断裂成立（罕见）每次都打；不成立（环形满溢，会按包频率命中）节流。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
            if (gap_reaches_break
                || (reanchor_probe_log_throttle_++ % config::JB_CONTROL_LOG_REANCHOR_PROBE_EVERY) == 0u) {
                log_debug_fmt(
                    "JitterBuffer reanchor probe: play_seq={} seq={} highest={} distance={} gap_from_highest={} min_gap={} capacity={} -> {}",
                    play, s, highest, distance,
                    s > highest ? (s - highest) : 0,
                    config::JB_REANCHOR_MIN_GAP_SLOTS, capacity_,
                    gap_reaches_break ? "request_reanchor"
                                      : "no-gap: ring overflow, deferred to deadline-high DROP");
            }
#endif
            if (gap_reaches_break) {
                if (distance > config::JB_MAX_REANCHOR_JUMP_FRAMES) {
                    // 控制面日志（#8）：sanity 拒绝必须给出具体跨度，否则只能
                    // 看到计数器涨、看不到"荒谬到什么程度"。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
                    log_warn_fmt(
                        "JitterBuffer reanchor sanity reject: seq={} play_seq={} span_frames={} max_span={} gap_from_highest={} -> rejected (absurd jump)",
                        s, play, distance, config::JB_MAX_REANCHOR_JUMP_FRAMES, s - highest);
#endif
                    reanchor_sanity_rejections_.fetch_add(1, std::memory_order_relaxed);
                    reanchor_sanity_pending_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_.fetch_add(1, std::memory_order_relaxed);
                    push_rejected_sanity_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                request_reanchor(s);
            }
            // 继续走正常的占用路径；在耗尽场景下，触发帧通常仍落在 EMPTY 槽，
            // 可以无损保留（reanchor 应用时只保留新窗口内 READY 槽，其余清掉）。
            // 已知残留边缘：顺序溢出（未达断裂缺口）且别名槽恰为空时，远端帧会暂占
            // 近端序列的槽；pull 在别名处按缺帧静音处理并回收该槽，最高水位的
            // deadline-high DROP 随后把时间线拉回 target，自愈为一帧静音 blip。
            // 不在此处直接拒绝——触发帧保留是 reanchor 快路径的前提（测试锁定）。
        }
    } else {
        const std::uint64_t oldest = oldest_seq_.load(std::memory_order_acquire);
        if (oldest != kNoOldestSeq) {
            if (s >= oldest && s - oldest >= capacity_ && s > highest
                && (s - highest) >= config::JB_REANCHOR_MIN_GAP_SLOTS) {
                // 控制面日志（#8）：启动前的远超前数据建立新的候选锚点。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
                log_debug_fmt(
                    "JitterBuffer reanchor probe (pre-start): seq={} oldest={} highest={} span_from_oldest={} gap_from_highest={} min_gap={} capacity={} -> request_reanchor + rebase oldest",
                    s, oldest, highest, s - oldest, s - highest,
                    config::JB_REANCHOR_MIN_GAP_SLOTS, capacity_);
#endif
                if (s - oldest > config::JB_MAX_REANCHOR_JUMP_FRAMES) {
                    // 控制面日志（#8）：见上面对 sanity 拒绝的说明。
#if AQUA_JB_CONTROL_THREAD_DEBUG_LOG
                    log_warn_fmt(
                        "JitterBuffer reanchor sanity reject (pre-start): seq={} oldest={} span_frames={} max_span={} -> rejected (absurd jump)",
                        s, oldest, s - oldest, config::JB_MAX_REANCHOR_JUMP_FRAMES);
#endif
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
        note_late_packet(play2 - s);
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
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
    const auto old_play = play_seq_.load(std::memory_order_relaxed);
    const auto old_highest = highest_seq_.load(std::memory_order_relaxed);
    const auto old_used = used_slots_.load(std::memory_order_relaxed);
    std::uint32_t removed_ready = 0;
#endif

    // 防越界：远超前触发帧通常无法落盘（其 ring 槽与未播放帧冲突被 busy 拒绝），
    // sequence 可能高于 highest。若直接把 play 跳到 sequence，会得到
    // play > highest 的静音洞，并触发 hold_until_target 的静音自持循环。
    // 这里把目标 clamp 到已落盘的活边 highest：既消除静音洞，又不越过实际
    // 收到的数据。last_reanchor_sequence_ 仍记录原始触发 sequence 供诊断。
    const auto highest_snapshot = highest_seq_.load(std::memory_order_acquire);
    const auto effective = sequence < highest_snapshot ? sequence : highest_snapshot;

    // 保留已落在新接收窗口内的 READY 帧，丢弃陈旧帧。
    // WRITING 槽不处理；若其 sequence 已落后于新播放时间线，
    // producer 侧的迟到复查会回收它们。
    for (std::uint32_t i = 0; i < capacity_; ++i) {
        auto& slot = slots_[i];
        if (slot.state.load(std::memory_order_acquire) != SlotState::Ready) {
            continue;
        }
        const auto q = slot.sequence;
        const bool in_window = q >= effective && (q - effective) < capacity_;
        if (!in_window) {
            SlotState expected = SlotState::Ready;
            if (slot.state.compare_exchange_strong(expected, SlotState::Empty,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                used_slots_.fetch_sub(1, std::memory_order_relaxed);

                // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
                ++removed_ready;
#endif
            }
        }
    }

    play_seq_.store(effective, std::memory_order_release);
    read_offset_ = 0;
    current_slot_ready_ = false;
    end_episode();
    episode_dir_ = EpisodeDir::Up;
    publish_episode_state(episode_dir_);
    fill_episodes_.fetch_add(1, std::memory_order_relaxed);
    hold_until_target_ = true;
    last_hold_lead_ = 0;
    hold_stuck_pulls_ = 0;
    // Phase 2：时间线被重置 → 旧 PCM 副本作废（不得跨不连续点重复），
    // 连续缺帧/掩盖 run 一并复位（新时间线重新开始计）。
    have_last_pcm_ = false;
    conceal_active_ = false;
    conceal_run_slots_ = 0;
    underrun_run_slots_ = 0;
    // 先存 sequence 再加 count：诊断 reader 按 count>0 取 last，顺序反了会读到哨兵 MAX。
    last_reanchor_sequence_.store(sequence, std::memory_order_release);
    reanchor_count_.fetch_add(1, std::memory_order_relaxed);
    snapshot_current();

    // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
    const auto new_highest = highest_seq_.load(std::memory_order_relaxed);
    const auto new_used = used_slots_.load(std::memory_order_relaxed);
    log_warn_fmt(
        "JitterBuffer water adjustment: REANCHOR play {}->{} highest {}->{} used {}->{} removed_ready={} hold_until_target=1",
        old_play == kNoPlaySeq ? 0 : old_play, effective,
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
    publish_episode_state(episode_dir_);
    consecutive_warning_ = 0;
    fill_repeat_slots_remaining_ = 0;
    fill_replaying_current_slot_ = false;
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

JitterBuffer::Action JitterBuffer::decide(std::uint64_t lead, std::uint32_t& skip_step) noexcept
{
    // 本拍只取一次 target 快照，并由它现算四个水位带：target 由 producer
    // 每个包都可能改写，分次读会让下面的判定用到不同时刻的阈值。
    const std::uint32_t target = target_slots();
    const Bands band = bands_from(target);
    if (episode_dir_ == EpisodeDir::Up) {
        if (lead >= target) {
            // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
            log_warn_fmt(
                "JitterBuffer water adjustment: FILL complete lead={}/{} target={} episode_steps={}",
                lead, capacity_, target, consecutive_warning_);
#endif
            end_episode();
            return Action::None;
        }
        if (hold_until_target_) {
            return Action::Hold;
        }
        if (fill_repeat_slots_remaining_ == 0 && !fill_replaying_current_slot_) {
            consecutive_warning_ += 1;
            const auto step = clamp_step(step_fn_(step_params_, consecutive_warning_));
            fill_repeat_slots_remaining_ = step;
            // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
            log_warn_fmt(
                "JitterBuffer water adjustment: FILL continue lead={}/{} target={} step={} repeat_slots={} episode_step={}",
                lead, capacity_, target, step, fill_repeat_slots_remaining_, consecutive_warning_);
#endif
        }
        return Action::Hold;
    }

    if (episode_dir_ == EpisodeDir::Down) {
        if (lead <= target) {
            // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
            log_warn_fmt(
                "JitterBuffer water adjustment: DROP complete lead={}/{} target={} episode_steps={}",
                lead, capacity_, target, consecutive_warning_);
#endif
            end_episode();
            return Action::None;
        }
        consecutive_warning_ += 1;
        skip_step = clamp_step(step_fn_(step_params_, consecutive_warning_));
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP lead={}/{} target={} step={} warning={}",
            lead, capacity_, target, skip_step, consecutive_warning_);
#endif
        return Action::Skip;
    }

    // 稳态
    if (lead < band.warning_low) {
        episode_dir_ = EpisodeDir::Up;
        publish_episode_state(episode_dir_);
        fill_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 0;
        hold_until_target_ = true;
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: FILL enter lead={}/{} warning_low={} target={} mode=hold_until_target",
            lead, capacity_, band.warning_low, target);
#endif
        return Action::Hold;
    }
    if (lead < band.normal_low) {
        episode_dir_ = EpisodeDir::Up;
        publish_episode_state(episode_dir_);
        fill_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 1;
        fill_repeat_slots_remaining_ = clamp_step(step_fn_(step_params_, 1));
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: FILL enter lead={}/{} normal_low={} target={} step={} repeat_slots={}",
            lead, capacity_, band.normal_low, target, consecutive_warning_, fill_repeat_slots_remaining_);
#endif
        return Action::Hold;
    }
    if (lead <= band.normal_high) {
        consecutive_warning_ = 0;
        return Action::None;
    }
    if (lead <= band.warning_high) {
        episode_dir_ = EpisodeDir::Down;
        drop_episodes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_warning_ = 1;
        skip_step = clamp_step(step_fn_(step_params_, 1));
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP enter lead={}/{} normal_high={} warning_high={} target={} step={}",
            lead, capacity_, band.normal_high, band.warning_high, target, skip_step);
#endif
        return Action::Skip;
    }
    // deadline 高：一步跳到 60%（步长封顶 N，防御异常跨度）。
    episode_dir_ = EpisodeDir::Down;
    drop_episodes_.fetch_add(1, std::memory_order_relaxed);
    consecutive_warning_ = 0;
    skip_step = static_cast<std::uint32_t>(std::min<std::uint64_t>(lead - target, capacity_));
    // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
    log_warn_fmt(
        "JitterBuffer water adjustment: DROP deadline-high lead={}/{} target={} step={}",
        lead, capacity_, target, skip_step);
#endif
    return Action::Skip;
}

JitterBufferPullResult JitterBuffer::pull(std::span<std::byte> output) noexcept
{
    JitterBufferPullResult result { };
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
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        const auto previous_deferred = deferred_reanchor_seq_.load(std::memory_order_relaxed);
#endif

        // deferred_reanchor_seq_ 是 consumer 私有状态（原子仅为让诊断线程可读），
        // 写入用显式 store：首次请求直接落盘，后续取两者更大（reanchor 目标单调取远）。
        const auto deferred_now = deferred_reanchor_seq_.load(std::memory_order_relaxed);
        if (deferred_now == kNoReanchorRequest || request > deferred_now) {
            deferred_reanchor_seq_.store(request, std::memory_order_relaxed);
        }
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: reanchor request received={} deferred={} previous_deferred={}",
            request, deferred_reanchor_seq_.load(std::memory_order_relaxed),
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
                const auto r = deferred_reanchor_seq_.load(std::memory_order_relaxed);
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
        const auto r = deferred_reanchor_seq_.load(std::memory_order_relaxed);
        deferred_reanchor_seq_ = kNoReanchorRequest;
        apply_reanchor(r);
        highest = highest_seq_.load(std::memory_order_acquire);
        play = play_seq_.load(std::memory_order_acquire);
    }

    if (play_seq_.load(std::memory_order_relaxed) == kNoPlaySeq) {
        const std::uint64_t oldest1 = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest1 = highest_seq_.load(std::memory_order_acquire);
        const std::uint64_t lead1 = (oldest1 <= highest1) ? (highest1 - oldest1 + 1) : 0;
        // 启动 pre-roll 水位 = startup_level（默认 50%，可调）：锚定即通知
        // 音频线程开始消费，为锚定后仍在涌入的帧留出 headroom——若等到
        // target，通知音频线程的间隙里网络推入可把低容量 JB 打满
        // （deadline-high Drop 抽搐）。锚定后 lead 位于 normal 区，
        // 稳态自然向 target 漂移，无需 FILL 干预。
        if (lead1 < startup_slots_) {
            std::fill(output.begin(), output.end(), silence_byte_);
            result.frames_filled = k;
            result.silence_frames = k;
            pull_frames_.fetch_add(k, std::memory_order_relaxed);
            pull_silence_frames_.fetch_add(k, std::memory_order_relaxed);
            record_silence_run(k);
            return result;
        }

        const std::uint64_t oldest2 = oldest_seq_.load(std::memory_order_acquire);
        const std::uint64_t highest2 = highest_seq_.load(std::memory_order_acquire);
        if (oldest1 != oldest2 || highest1 != highest2) {
            std::fill(output.begin(), output.end(), silence_byte_);
            result.frames_filled = k;
            result.silence_frames = k;
            pull_frames_.fetch_add(k, std::memory_order_relaxed);
            pull_silence_frames_.fetch_add(k, std::memory_order_relaxed);
            record_silence_run(k);
            return result;
        }

        play_seq_.store(oldest2, std::memory_order_release);
        read_offset_ = 0;
        snapshot_current();
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_debug_fmt(
            "JitterBuffer timeline anchor established: play_seq={} lead={}/{} startup_level(startup)={} used_slots={}",
            oldest2, lead1, capacity_, startup_slots_, used_slots_.load(std::memory_order_relaxed));
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

        if (hold_stuck_pulls_ >= config::JB_REANCHOR_HOLD_STUCK_PULLS) {
            // for debug jitter buffer stat（RT 侧埋点：兜底路径此前完全静默，
            // 事后只能从 reanchor_count 的跳变猜测它是否触发过）。
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
            log_warn_fmt(
                "JitterBuffer reanchor hold-stuck fallback: pulls={} lead={} last_lead={} threshold={} target={} play_seq={} highest={}",
                hold_stuck_pulls_, lead, last_hold_lead_,
                config::JB_REANCHOR_HOLD_STUCK_PULLS, target_slots(),
                play_seq_.load(std::memory_order_relaxed),
                highest_seq_.load(std::memory_order_relaxed));
#endif
            const auto r = deferred_reanchor_seq_.load(std::memory_order_relaxed);
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

    if (action == Action::Hold && hold_until_target_) {
        // deadline-low / reanchor recovery：必须保证输出可播放，因此继续以静音停住 play_seq。
        std::fill(output.begin(), output.end(), silence_byte_);
        result.frames_filled = k;
        result.silence_frames = k;
        pull_frames_.fetch_add(k, std::memory_order_relaxed);
        pull_silence_frames_.fetch_add(k, std::memory_order_relaxed);
        record_silence_run(k);
        return result;
    }

    if (action == Action::Skip) {
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        const auto before_skip = play_seq_.load(std::memory_order_relaxed);
#endif
        for (std::uint32_t i = 0; i < skip_step; ++i) {
            advance_slot();
        }
        result.skipped_slots = skip_step;
        drop_skipped_slots_.fetch_add(skip_step, std::memory_order_relaxed);
        // for debug jitter buffer stat.
#if AQUA_JB_RUNTIME_THREAD_DEBUG_LOG
        log_warn_fmt(
            "JitterBuffer water adjustment: DROP applied play_seq {}->{} skipped_slots={} used_slots={} lead_before={}",
            before_skip, play_seq_.load(std::memory_order_relaxed), skip_step,
            used_slots_.load(std::memory_order_relaxed), lead);
#endif
    }

    std::uint32_t filled = 0;
    std::uint32_t silence = 0;
    std::uint32_t concealed = 0; // Phase 2：本次 pull 中被 repeat-last 掩盖的帧数
    while (filled < k) {
        const std::uint64_t p = play_seq_.load(std::memory_order_relaxed);
        if (p > highest) {
            // JB 排空：时间线已越过已收到的最高序列，没有数据可播。play_seq
            // 不推进（守卫语义不变：停住等数据，而不是追着空气跑）。
            // Phase 2 扩展：排空的每 frame_count_ 帧视为一个缺失 slot，与"洞"
            // 路径共用 conceal 决策（repeat-last + 线性淡出 + 饱和封顶）与
            // underrun run 统计——排空的包同样是 missing packet（细则 §9），
            // 否则 burst 到达模式下 underrun 全部走此路径，concealment 永远
            // 没有机会生效（双机实测：underrun_events 持续增长但 conceal=0）。
            const std::uint32_t remain = k - filled;
            std::uint32_t done = 0;
            while (done < remain) {
                const std::uint32_t n_slot = std::min(frame_count_, remain - done);
                // 复用缺帧 slot 判定：记 underrun run（含 max 更新）、按连续
                // 长度决定本 slot 是掩盖还是静音/饱和。
                on_slot_boundary(false);
                std::byte* dst = output.data()
                    + static_cast<std::size_t>(filled + done) * frame_bytes_;
                if (conceal_active_) {
                    // 整包重复（尾块不足一包时取头部：即将饱和转静音或被新
                    // 数据接续，"包头循环"的听感优于硬静音）。
                    std::copy_n(last_pcm_.data(),
                        static_cast<std::size_t>(n_slot) * frame_bytes_, dst);
                    scale_frames(dst, n_slot, conceal_gain_q15_);
                    concealed += n_slot;
                } else {
                    std::fill_n(dst, static_cast<std::size_t>(n_slot) * frame_bytes_,
                        silence_byte_);
                    silence += n_slot;
                }
                done += n_slot;
            }
            filled = k;
            break;
        }

        const std::uint32_t idx = static_cast<std::uint32_t>(p % capacity_);
        const std::uint32_t n = std::min(frame_count_ - read_offset_, k - filled);
        if (read_offset_ == 0) {
            // 槽边界刷新就绪快照：上方 p > highest 的静音守卫路径（underrun
            // 排空后）不更新 current_slot_ready_，其残留 false 会把恰好落在
            // play_seq 的新 READY 槽误判为静音并 advance 跳过；若 producer 与
            // pull 形成 1:1 步进，每帧都被同样跳过 → 播放头在静音中前行、
            // 真实数据全部丢失（underrun 恢复静默饿死）。部分槽读取期间槽
            // 不会被回收（advance 才回收），故只需在槽边界刷新。
            snapshot_current();
            // Phase 2：槽边界一次性决定本 slot 是"掩盖"还是"静音"，并结算
            // underrun run（一个 slot 只判一次，跨 pull 的部分消费沿用决策）。
            on_slot_boundary(current_slot_ready_);
        }
        if (current_slot_ready_) {
            const std::byte* src = slot_data(idx)
                + static_cast<std::size_t>(read_offset_) * frame_bytes_;
            std::copy_n(src, static_cast<std::size_t>(n) * frame_bytes_,
                output.data() + static_cast<std::size_t>(filled) * frame_bytes_);
            if (conceal_enabled_) {
                // 保存本 slot 的真实 PCM 副本（定长 memcpy，slot 边界内累积成
                // 完整一包）：掩盖只能重复"已经播过的最后一个真实包"。
                std::copy_n(src, static_cast<std::size_t>(n) * frame_bytes_,
                    last_pcm_.data() + static_cast<std::size_t>(read_offset_) * frame_bytes_);
                have_last_pcm_ = true;
            }
        } else if (conceal_active_) {
            // Phase 2 掩盖：重复上一包 + 短淡出（不修改 sequence/timestamp）。
            std::copy_n(last_pcm_.data() + static_cast<std::size_t>(read_offset_) * frame_bytes_,
                static_cast<std::size_t>(n) * frame_bytes_,
                output.data() + static_cast<std::size_t>(filled) * frame_bytes_);
            scale_frames(output.data() + static_cast<std::size_t>(filled) * frame_bytes_,
                n, conceal_gain_q15_);
            concealed += n;
        } else {
            std::fill_n(output.data() + static_cast<std::size_t>(filled) * frame_bytes_,
                static_cast<std::size_t>(n) * frame_bytes_, silence_byte_);
            silence += n;
        }
        filled += n;
        read_offset_ += n;
        if (read_offset_ == frame_count_) {
            if (action == Action::Hold && current_slot_ready_) {
                if (fill_replaying_current_slot_) {
                    // We have just finished one replay of the current slot. Start another
                    // replay if the warning step still has budget; otherwise advance normally.
                    if (fill_repeat_slots_remaining_ > 0) {
                        --fill_repeat_slots_remaining_;
                        fill_corrected_slots_.fetch_add(1, std::memory_order_relaxed);
                        read_offset_ = 0;
                        snapshot_current();
                    } else {
                        fill_replaying_current_slot_ = false;
                        advance_slot();
                    }
                } else if (fill_repeat_slots_remaining_ > 0) {
                    // The original pass over this slot is complete. Consume one replay
                    // budget and replay the same READY slot once.
                    --fill_repeat_slots_remaining_;
                    fill_replaying_current_slot_ = true;
                    fill_corrected_slots_.fetch_add(1, std::memory_order_relaxed);
                    read_offset_ = 0;
                    snapshot_current();
                } else {
                    advance_slot();
                }
            } else {
                advance_slot();
            }
        }
    }

    result.frames_filled = filled;
    result.silence_frames = silence;
    pull_frames_.fetch_add(filled, std::memory_order_relaxed);
    pull_silence_frames_.fetch_add(silence, std::memory_order_relaxed);
    // underrun = 没有真实 PCM 可用的帧：掩盖帧 + 缺帧静音。不含 pre-roll /
    // 低水位 Hold（时间轴修正）与 Drop 跳过的帧——那两种在 pull 前半段已
    // 提前 return / 不产生输出。
    const std::uint32_t underrun = silence + concealed;
    if (underrun != 0) {
        underrun_frames_.fetch_add(underrun, std::memory_order_relaxed);
    }
    record_silence_run(silence);
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
    fill_corrected_slots_.store(0, std::memory_order_relaxed);
    drop_episodes_.store(0, std::memory_order_relaxed);
    drop_skipped_slots_.store(0, std::memory_order_relaxed);
    reanchor_requests_.store(0, std::memory_order_relaxed);
    reanchor_cancels_.store(0, std::memory_order_relaxed);
    reanchor_count_.store(0, std::memory_order_relaxed);
    reanchor_sanity_rejections_.store(0, std::memory_order_relaxed);
    reanchor_sanity_pending_.store(0, std::memory_order_relaxed);
    last_reanchor_sequence_.store(kNoReanchorRequest, std::memory_order_relaxed);
    consecutive_silence_frames_.store(0, std::memory_order_relaxed);
    max_silence_run_frames_.store(0, std::memory_order_relaxed);
    episode_state_.store(0, std::memory_order_relaxed);
    read_offset_ = 0;
    current_slot_ready_ = false;
    episode_dir_ = EpisodeDir::None;
    consecutive_warning_ = 0;
    fill_repeat_slots_remaining_ = 0;
    fill_replaying_current_slot_ = false;
    hold_until_target_ = false;
    deferred_reanchor_seq_ = kNoReanchorRequest;
    last_hold_lead_ = 0;
    hold_stuck_pulls_ = 0;
    // Phase 2 concealment / 欠载状态
    have_last_pcm_ = false;
    conceal_active_ = false;
    conceal_gain_q15_ = 0;
    conceal_run_slots_ = 0;
    underrun_run_slots_ = 0;
    // 镜像一并归零：reset 由控制线程调用（producer/consumer 均已停止），
    // 不归零会让诊断在复位后仍显示上一次会话的 run 长度。
    conceal_run_slots_out_.store(0, std::memory_order_relaxed);
    underrun_run_slots_out_.store(0, std::memory_order_relaxed);
    underrun_events_.store(0, std::memory_order_relaxed);
    underrun_frames_.store(0, std::memory_order_relaxed);
    max_consecutive_underrun_slots_.store(0, std::memory_order_relaxed);
    concealed_slots_.store(0, std::memory_order_relaxed);
    concealed_saturated_slots_.store(0, std::memory_order_relaxed);
    late_useful_packets_.store(0, std::memory_order_relaxed);
}

} // namespace aqua::audio
