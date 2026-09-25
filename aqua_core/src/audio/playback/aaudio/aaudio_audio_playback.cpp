// AAudio 回放后端实现。设计决议：aqua_core/doc/aaudio_backend_design.md。

#include "audio/playback/aaudio/aaudio_audio_playback.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"
#include "audio/devices/aaudio/aaudio_device_manager.h"
#include "audio/public/aaudio/aaudio_error.h"
#include "audio/public/aaudio/aaudio_format.h"
#include "audio/public/audio_fill_silence.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>

// RT（AAudio data callback）诊断日志开关：默认关闭。data callback 运行在
// AAudio 的实时线程上，spdlog 内部有锁，同步调用会破坏实时契约；仅离线排查
// 时临时置 1。不覆盖 on_error_callback：按 AAudio 语义它由独立的错误回调
// 线程投递（见 playback.md / audio_playback.h），非 RT 渲染线程。
#ifndef AQUA_CLIENT_RT_DEBUG_LOG
#define AQUA_CLIENT_RT_DEBUG_LOG 0
#endif

namespace aqua::audio::aaudio {
namespace {

    // AudioStreamInfo 统一词汇与 AAudio 原生枚举数值锁定（诊断契约）。
    static_assert(AAUDIO_PERFORMANCE_MODE_NONE == AudioStreamInfo::kPerformanceNone,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");
    static_assert(AAUDIO_PERFORMANCE_MODE_LOW_LATENCY == AudioStreamInfo::kPerformanceLowLatency,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");
    static_assert(AAUDIO_PERFORMANCE_MODE_POWER_SAVING == AudioStreamInfo::kPerformancePowerSaving,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");

} // namespace

AAudioAudioPlayback::AAudioAudioPlayback(AudioDeviceManager& device_manager)
    : device_manager_(device_manager)
{
    log_debug("AAudio playback backend instance created");
}

AAudioAudioPlayback::~AAudioAudioPlayback()
{
    stop();
}

std::expected<void, AudioError> AAudioAudioPlayback::start(
    const AudioPlaybackConfig& config,
    AudioPlaybackCallback callback,
    AudioPlaybackEventCallback event_callback) noexcept
{
    // 路由请求（devices/audio_route.h）：跟随系统 = 不带端点 id，AudioPolicy
    // 保有路由权；Application = 绑定到具体 id。
    const auto requested_device = config.route.endpoint_request();
    log_debug_fmt("AAudio playback config: route={} device={} format={}ch/{}Hz/enc={} buffer_frames={} low_latency={}",
        audio_route_authority_name(config.route.authority),
        requested_device ? requested_device->value() : std::string("system"),
        config.format.channels, config.format.sample_rate, static_cast<int>(config.format.encoding),
        config.frames_per_buffer, config.low_latency);

    // 半死流防护：report_fatal_once() 把 running_ 置 false 但不 close
    //（close 留给控制线程 stop()，避免在回调线程内关流死锁）。若不经 stop()
    // 直接 start()，旧 stream_ 会被覆盖泄漏且旧 data 回调仍在飞。
    // 对齐 WASAPI 两后端的 `running_ || thread.joinable()` 门禁。
    if (running_.load(std::memory_order_acquire) || stream_ != nullptr) {
        log_error("AAudio playback: start rejected because playback is already running");
        return std::unexpected(AudioError::AlreadyRunning);
    }
    if (!callback) {
        log_error("AAudio playback: start rejected because callback is empty");
        return std::unexpected(AudioError::InvalidArgument);
    }
    if (!config.format.is_valid()) {
        log_error("AAudio playback: start rejected because requested format is invalid");
        return std::unexpected(AudioError::InvalidArgument);
    }

    const auto requested_format = to_aaudio_format(config.format.encoding);
    if (!requested_format) {
        log_error_fmt("AAudio playback: encoding {} has no AAudio representation",
            static_cast<int>(config.format.encoding));
        return std::unexpected(requested_format.error());
    }

    // resolve 仅用于设备方向校验与日志；实际是否绑定端点由下面的
    // setDeviceId 分支按路由权威决定。
    const auto resolved = device_manager_.resolve(AudioDeviceDirection::OUTPUT, requested_device);
    if (!resolved) {
        log_error_fmt("AAudio playback: device resolution failed: {}", audio_error_name(resolved.error()));
        return std::unexpected(resolved.error());
    }
    log_debug_fmt("AAudio playback device resolved: id='{}' name='{}' default={}",
        resolved->id.value(), resolved->name, resolved->is_default);

    AAudioStreamBuilder* raw_builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&raw_builder);
    if (result != AAUDIO_OK || raw_builder == nullptr) {
        log_error_fmt("AAudio playback: createStreamBuilder failed: {} ({})",
            aaudio_result_name(result), static_cast<int>(result));
        return std::unexpected(map_aaudio_error(result));
    }

    // builder RAII：所有退出路径统一 delete。无状态 deleter 的 unique_ptr，
    // 取代局部 BuilderGuard（后者可拷贝、builder 未判空）。
    struct BuilderDeleter {
        void operator()(AAudioStreamBuilder* builder) const noexcept
        {
            if (builder != nullptr) {
                AAudioStreamBuilder_delete(builder);
            }
        }
    };
    const std::unique_ptr<AAudioStreamBuilder, BuilderDeleter> builder { raw_builder };

    // 契约格式全量下发；采样率是否被系统 SRC 由回读校验决定（设计决议 §1）。
    AAudioStreamBuilder_setFormat(raw_builder, *requested_format);
    AAudioStreamBuilder_setChannelCount(raw_builder, static_cast<int32_t>(config.format.channels));
    AAudioStreamBuilder_setSampleRate(raw_builder, static_cast<int32_t>(config.format.sample_rate));
    AAudioStreamBuilder_setDirection(raw_builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(
        raw_builder,
        config.low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(raw_builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(raw_builder, AAUDIO_USAGE_MEDIA);
    // 播放设备路由（playback_switching_design.md §8）：Application 权威下把
    // "android:N" 编码的显式设备映射为 setDeviceId；System 权威**不设**——
    // 这正是让系统输出切换器保有路由权的原因（设计决议 §3.1）。
    //
    // 注意 §3.1 记录的平台事实边界：setDeviceId 仅对 USB/BT 外接设备可靠，
    // 对内建设备（扬声器/听筒）行为未定义。钉住内建设备时 AudioPolicy 可能
    // 立刻把流断开，这由 PlaybackManager 的 settle 预算吸收（见
    // playback_manager.h 的 kRouteSettleWindow），backend 不做特判。
    if (config.route.is_pinned()) {
        const auto aaudio_device = parse_aaudio_device_id(*requested_device);
        if (aaudio_device.has_value()) {
            AAudioStreamBuilder_setDeviceId(raw_builder, *aaudio_device);
            log_debug_fmt("AAudio playback: routing to explicit device id {}",
                *aaudio_device);
        }
        // 非法格式已在 resolve() 阶段拒绝，这里防御性跳过。
    }
    // framesPerCallback 自适应（设计决议 §2）：不设固定回调粒度，
    // AAudio 按设备原生 burst 分发，JitterBuffer pre-roll 水位最准。
    // config.frames_per_buffer 仅作为 buffer 容量提示（0 = 系统默认 2× burst）。
    if (config.frames_per_buffer != 0) {
        AAudioStreamBuilder_setBufferCapacityInFrames(
            raw_builder, static_cast<int32_t>(config.frames_per_buffer));
    }

    // ---- 流身份槽位（user_data）----
    // 必须在 setDataCallback 之前建好：AAudio 只透传 user_data，回调靠它拿到
    // 自己那条流的 generation 与上下文。槽位由本类持有（current -> retired），
    // 生命周期覆盖迟到回调窗口，详见头文件 StreamSlot / retired_slot_ 注释。
    try {
        auto slot = std::make_unique<StreamSlot>();
        slot->self = this;
        slot->generation = generation_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
        slot->context = std::make_shared<CallbackContext>();
        slot->context->callback = std::move(callback);
        current_slot_ = std::move(slot);
    } catch (...) {
        current_slot_.reset();
        return std::unexpected(AudioError::BackendFailed);
    }
    AAudioStreamBuilder_setDataCallback(
        raw_builder, &AAudioAudioPlayback::on_data_callback, current_slot_.get());
    AAudioStreamBuilder_setErrorCallback(
        raw_builder, &AAudioAudioPlayback::on_error_callback, current_slot_.get());

    event_callback_ = std::move(event_callback);
    pending_error_.store(AudioError::None, std::memory_order_release);
    fatal_reported_.store(false, std::memory_order_release);

    AAudioStream* raw_stream = nullptr;
    result = AAudioStreamBuilder_openStream(raw_builder, &raw_stream);
    if (result != AAUDIO_OK || raw_stream == nullptr) {
        log_error_fmt("AAudio playback: openStream failed: {} ({})",
            aaudio_result_name(result), static_cast<int>(result));
        current_slot_.reset();
        event_callback_ = nullptr;
        return std::unexpected(map_aaudio_error(result));
    }
    // stream RAII：openStream 成功即接管——下面回读校验 / requestStart 的四个
    // 失败分支不再各自重复 AAudioStream_close 三连。此刻 stream 尚未 start，
    // deleter 只需 close（无需 requestStop）。
    struct StreamDeleter {
        void operator()(AAudioStream* stream) const noexcept
        {
            if (stream != nullptr) {
                AAudioStream_close(stream);
            }
        }
    };
    // 非 const：下面 requestStart 成功后要 release() 移交成员。
    std::unique_ptr<AAudioStream, StreamDeleter> opened_stream { raw_stream };

    // ---- 回读实际 stream 配置并做字节契约硬校验（设计决议 §1）----
    const auto actual_format = from_aaudio_format(AAudioStream_getFormat(raw_stream));
    const auto actual_channels = static_cast<std::uint32_t>(AAudioStream_getChannelCount(raw_stream));
    const auto actual_rate = static_cast<std::uint32_t>(AAudioStream_getSampleRate(raw_stream));

    if (actual_format != config.format.encoding) {
        log_error_fmt("AAudio playback: actual encoding {} != requested {} (rejected: byte-layout contract)",
            static_cast<int>(actual_format.value_or(AudioEncoding::INVALID)),
            static_cast<int>(config.format.encoding));
        current_slot_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::FormatUnsupported);
    }
    if (actual_channels != config.format.channels) {
        log_error_fmt("AAudio playback: actual channels {} != requested {} (rejected: remix semantics uncontrolled)",
            actual_channels, config.format.channels);
        current_slot_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::FormatUnsupported);
    }
    if (actual_rate != config.format.sample_rate) {
        // 采样率允许系统 SRC：JitterBuffer 水位机制吸收漂移（设计决议 §1.2）。
        log_info_fmt("AAudio playback: sample rate adjusted by system SRC: {} -> {} Hz",
            config.format.sample_rate, actual_rate);
    }

    current_slot_->context->frame_bytes = config.format.frame_bytes();
    current_slot_->context->silence_byte = config.format.silence_byte();
    if (current_slot_->context->frame_bytes == 0) {
        log_error("AAudio playback: frame_bytes resolved to 0");
        current_slot_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::InvalidArgument);
    }

    // 认领这条流：必须在 requestStart **之前**发布，失败/断开回调可能与状态
    // 迁移竞争。此后只有携带该 generation 的回调能修改共享状态，任何旧流的
    // 迟到讣告都在 report_fatal_once 的 generation 检查处被挡下。
    live_generation_.store(current_slot_->generation, std::memory_order_release);
    result = AAudioStream_requestStart(raw_stream);
    if (result != AAUDIO_OK) {
        log_error_fmt("AAudio playback: requestStart failed: {} ({})",
            aaudio_result_name(result), static_cast<int>(result));
        live_generation_.store(kNoStreamGeneration, std::memory_order_release);
        current_slot_.reset();
        event_callback_ = nullptr;
        return std::unexpected(map_aaudio_error(result));
    }

    // requestStart 成功才把所有权移交成员（此后 stop() 负责 requestStop+close）。
    stream_ = opened_stream.release();
    running_.store(true, std::memory_order_release);

    // ---- 回读实际 stream 运行参数：日志 + 诊断缓存（一次性快照）----
    // 不读取 sharing mode（项目明确仅 SHARED）、buffer size（不调用
    // setBufferSizeInFrames，size 恒等于容量）与 callback_frames（未设
    // setFramesPerCallback，回读恒为 unspecified）。
    const auto performance_mode = AAudioStream_getPerformanceMode(raw_stream);
    const auto frames_per_burst = AAudioStream_getFramesPerBurst(raw_stream);
    const auto capacity = AAudioStream_getBufferCapacityInFrames(raw_stream);
    // 实际输出设备回读（playback_switching_design.md §8）：UNSPECIFIED 时
    // 留空（未知）；restart 事务的 previous_active_device 由此捕获。
    const auto actual_device_id = AAudioStream_getDeviceId(raw_stream);
    if (actual_device_id != AAUDIO_UNSPECIFIED) {
        std::lock_guard lock(info_device_mutex_);
        info_device_id_ = encode_aaudio_device_id(actual_device_id);
    }

    info_sample_rate_.store(actual_rate, std::memory_order_relaxed);
    info_channels_.store(actual_channels, std::memory_order_relaxed);
    info_performance_mode_.store(performance_mode, std::memory_order_relaxed);
    info_frames_per_burst_.store(
        static_cast<std::uint32_t>(frames_per_burst), std::memory_order_relaxed);
    info_buffer_capacity_.store(
        static_cast<std::uint32_t>(capacity), std::memory_order_relaxed);

    log_info_fmt("AAudio playback started: performance={} frames_per_burst={} capacity={} format={}ch/{}Hz (requested {}Hz)",
        audio_stream_performance_name(performance_mode),
        frames_per_burst, capacity,
        actual_channels, actual_rate, config.format.sample_rate);
    return { };
}

bool AAudioAudioPlayback::is_running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

AudioStreamInfo AAudioAudioPlayback::stream_info() const noexcept
{
    // sample_rate=0 表示尚未 start（或已 stop 清零）→ backend=None。
    if (info_sample_rate_.load(std::memory_order_relaxed) == 0) {
        return { };
    }
    AudioStreamInfo info;
    info.backend = AudioStreamInfo::Backend::AAudio;
    info.sample_rate = info_sample_rate_.load(std::memory_order_relaxed);
    info.channels = info_channels_.load(std::memory_order_relaxed);
    info.performance_mode = info_performance_mode_.load(std::memory_order_relaxed);
    info.frames_per_burst = info_frames_per_burst_.load(std::memory_order_relaxed);
    info.buffer_capacity_frames = info_buffer_capacity_.load(std::memory_order_relaxed);
    // 运行期统计（Gauge）：data callback 次数 + xRun（AAudio 侧欠载）。
    info.callback_count = stats_callback_count_.load(std::memory_order_relaxed);
    info.xrun_count = stats_xrun_count_.load(std::memory_order_relaxed);
    try {
        std::lock_guard lock(info_device_mutex_);
        info.device_id = AudioDeviceId(info_device_id_);
    } catch (...) {
        // lock 失败理论上不可达；device_id 留空即可（noexcept 契约优先）。
    }
    return info;
}

void AAudioAudioPlayback::stop() noexcept
{
    log_debug("AAudio playback stop requested");
    if (!running_.load(std::memory_order_acquire) && stream_ == nullptr) {
        return;
    }

    // 撤销认领必须是**第一件事**：此后任何迟到回调都在 report_fatal_once 的
    // generation 检查处被挡下，不会去触碰下面即将清空的 event_callback_。
    // 记下被停掉的世代，供 stop 路径投递 pending error 时打戳（manager 会把
    // 它与自己已撤销的认领比对后丢弃——teardown 期间的事件由事务自己负责）。
    const StreamGeneration stopping_generation
        = live_generation_.exchange(kNoStreamGeneration, std::memory_order_acq_rel);

    if (stream_ != nullptr) {
        // requestStop 停止 data callback 调度；close 隐含 stop 并等待在途回调
        // 返回（AAudio 同步语义）。回调内不做任何 close——死锁约束由本控制
        // 线程独占执行（设计决议 §5）。
        (void)AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }

    // close 已返回，data callback 不再使用 current_slot_。降级为 retired，
    // 覆盖 error callback 与 close 不同步的迟到窗口；上一代 retired 在此释放。
    // （start 失败路径下 current_slot_ 为空，此时不能动 retired——它可能还在
    // 为上一条流的迟到回调兜底。）
    if (current_slot_) {
        retired_slot_ = std::move(current_slot_);
    }

    // error callback 发布的 pending error（若有）在此投递：stop 路径的
    // event_callback_ 调用发生在控制线程，满足"不在回调线程内调 stop"契约。
    // 已即时投递过的错误（report_fatal_once）不重复投递，避免一次错误触发
    // 两次错误驱动恢复（多余的 stop/start 会拉长静音窗口）。
    const AudioError error = pending_error_.exchange(AudioError::None, std::memory_order_acq_rel);
    const bool already_reported = fatal_reported_.load(std::memory_order_acquire);
    // 先置位再清空回调：即便有 error callback 线程恰好越过了 generation 检查
    // （撤销认领与本行之间），其 fatal_reported_.exchange(true) 也会拿到 true
    // 直接返回，从而不会去调用下面即将被析构的 event_callback_（TOCTOU）。
    fatal_reported_.store(true, std::memory_order_release);
    if (error != AudioError::None && !already_reported) {
        log_debug_fmt("AAudio playback stopped with error: {} (stream generation {})",
            audio_error_name(error), stopping_generation);
        if (event_callback_) {
            try {
                event_callback_(AudioStreamEvent { error, stopping_generation });
            } catch (...) {
                log_error("AAudio playback event callback exception");
            }
        }
    }

    event_callback_ = nullptr;
    // 回调已清空，此时才解除致命错误上报的占用，供下一次 start() 使用。
    fatal_reported_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // 诊断缓存清零：stream_info() 回到 backend=None。
    info_sample_rate_.store(0, std::memory_order_relaxed);
    info_channels_.store(0, std::memory_order_relaxed);
    info_performance_mode_.store(0, std::memory_order_relaxed);
    info_frames_per_burst_.store(0, std::memory_order_relaxed);
    info_buffer_capacity_.store(0, std::memory_order_relaxed);
    stats_callback_count_.store(0, std::memory_order_relaxed);
    stats_xrun_count_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(info_device_mutex_);
        info_device_id_.clear();
    }

    log_debug_fmt("AAudio playback stopped (stale events dropped so far: {})",
        stale_events_dropped_.load(std::memory_order_relaxed));
}

void AAudioAudioPlayback::publish_error(AudioError error) noexcept
{
    if (error == AudioError::None) {
        return;
    }
    pending_error_.store(error, std::memory_order_release);
}

void AAudioAudioPlayback::report_fatal_once(const StreamSlot& slot, AudioError error) noexcept
{
    if (error == AudioError::None) {
        return;
    }
    // ---- 流归属边界 ----
    // 只有当前被认领的流才允许修改共享状态。AAudio 的 error callback 不与
    // close() 同步，已退役流的迟到讣告真实存在；一旦放行，它会把复用了本
    // 实例的新流标记为已死（running_ = false + 投递事件），上层据此拆掉一条
    // 健康的流。令牌是 slot 自带的 generation 而非 AAudioStream* 指针——
    // 指针在 close 之后可能被新流复用（ABA），不能当身份。
    if (slot.generation != live_generation_.load(std::memory_order_acquire)) {
        stale_events_dropped_.fetch_add(1, std::memory_order_relaxed);
        // 本函数可由 data callback（RT 线程）调用，同步日志必须门控。
#if AQUA_CLIENT_RT_DEBUG_LOG
        log_debug_fmt("AAudio playback: dropped {} from retired stream generation {}",
            audio_error_name(error), slot.generation);
#endif
        return;
    }
    publish_error(error);
    // 流已死（或即将被 data callback STOP）：is_running 必须如实反映，
    // 否则诊断/监督看到 Running 的假阳性，静默死流无法被兜底检测。
    running_.store(false, std::memory_order_release);
    // 与 WASAPI 事件线程对等的即时投递：运行期错误立刻进入 ClientRuntime
    // 的错误驱动恢复（asio::post 到 ioc），不等 stop() 才投递。
    if (!event_callback_) {
        return;
    }
    if (fatal_reported_.exchange(true, std::memory_order_acq_rel)) {
        return; // 另一回调路径已投递过本次错误
    }
    // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）：本函数可由
    // data callback 调用，fatal_reported_ 保证每条流至多投递一次。
#if AQUA_CLIENT_RT_DEBUG_LOG
    log_debug_fmt("AAudio playback: dispatching runtime error event: {} (generation {})",
        audio_error_name(error), slot.generation);
#endif
    try {
        event_callback_(AudioStreamEvent { error, slot.generation });
    } catch (...) {
        // 本函数可由 data callback（RT 线程）调用：同步日志有锁 + IO，必须门控。
#if AQUA_CLIENT_RT_DEBUG_LOG
        log_error("AAudio playback event callback exception");
#endif
    }
}

aaudio_data_callback_result_t AAudioAudioPlayback::on_data_callback(
    AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) noexcept
{
    // user_data 是**本条流**的 StreamSlot（start() 时传入，由 backend 持有到
    // retired 世代为止）。回调只信任自己拿到的槽位，不从 self 上读"当前"
    // 上下文——那正是流身份丢失的来源。上下文经 shared_ptr 持有：即使 close
    // 与本回调竞争，callback 对象依然保活。
    auto* slot = static_cast<StreamSlot*>(user_data);
    if (slot == nullptr || slot->self == nullptr || num_frames <= 0) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    auto* self = slot->self;
    const auto context = slot->context;
    if (context == nullptr) {
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // 运行期统计：data callback 次数；xrun 每 64 次回调采样一次
    // （RT 上节流——getXRunCount 是轻量读，仍不每次调用）。负数 = 错误，忽略。
    const auto calls = self->stats_callback_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls & 0x3F) == 0) {
        const auto xrun = AAudioStream_getXRunCount(stream);
        if (xrun >= 0) {
            self->stats_xrun_count_.store(static_cast<std::uint64_t>(xrun),
                std::memory_order_relaxed);
        }
    }

    const std::size_t output_bytes = static_cast<std::size_t>(num_frames) * context->frame_bytes;
    const std::span<std::byte> output(static_cast<std::byte*>(audio_data), output_bytes);

    std::uint32_t written_frames = 0;
    if (context->callback) {
        // 回调契约：noexcept 语义由 pull 侧保证（JitterBuffer pull 不抛）；
        // 兜底捕获任何异常，静音填充、上报致命错误并停止分发。
        try {
            written_frames = context->callback(output);
        } catch (...) {
            // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）。
#if AQUA_CLIENT_RT_DEBUG_LOG
            log_error("AAudio playback data callback exception");
#endif
            std::ranges::fill(output, context->silence_byte);
            self->report_fatal_once(*slot, AudioError::BackendFailed);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
    }

    if (written_frames > static_cast<std::uint32_t>(num_frames)) {
        // RT 线程日志（见本文件顶部 AQUA_CLIENT_RT_DEBUG_LOG）。
#if AQUA_CLIENT_RT_DEBUG_LOG
        log_error_fmt("AAudio playback callback returned {} frames, but only {} requested",
            written_frames, num_frames);
#endif
        std::ranges::fill(output, context->silence_byte);
        self->report_fatal_once(*slot, AudioError::BackendFailed);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // 契约：未填满部分补静音，避免复用残留数据（audio_playback.h 头注释）。
    fill_silence_tail(output,
        static_cast<std::size_t>(written_frames) * context->frame_bytes,
        context->silence_byte);

    if (self->pending_error_.load(std::memory_order_acquire) != AudioError::None) {
        // error callback 已通过 report_fatal_once 即时上报；这里只停流。
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioAudioPlayback::on_error_callback(
    AAudioStream* stream, void* user_data, aaudio_result_t error) noexcept
{
    // 身份来自 slot 携带的 generation，而非 stream 指针：指针在 close 之后
    // 可能被新流复用（ABA），不能当令牌。
    (void)stream;
    auto* slot = static_cast<StreamSlot*>(user_data);
    if (slot == nullptr || slot->self == nullptr) {
        return;
    }
    const AudioError mapped = map_aaudio_error(error);
    // 两个 generation 都打出来：本次 Android 断连之所以难定位，正是因为日志
    // 无法回答"这条 DISCONNECTED 属于哪条流"。
    log_warn_fmt("AAudio playback error callback: {} ({}) -> {} stream_generation={} live_generation={}",
        aaudio_result_name(error), static_cast<int>(error), audio_error_name(mapped),
        slot->generation, slot->self->generation());

    // 发布 pending error 并即时投递事件（不 close/stop，设计决议 §5）：
    // data callback 随后观察到 pending_error_ 自行返回 STOP；event 投递
    // 驱动 ClientRuntime 立即在 ioc 线程执行 restart 事务（与 WASAPI
    // 事件线程对等）。若只在 stop() 投递，流死后 runtime 无从感知——
    // 表现为 JB 被网络侧打满、输出永久静音。
    slot->self->report_fatal_once(*slot, mapped);
}

} // namespace aqua::audio::aaudio
