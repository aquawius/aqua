// AAudio 回放后端实现。设计决议：aqua_core/doc/aaudio_backend_design.md。

#include "audio/playback/aaudio/aaudio_audio_playback.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"
#include "audio/devices/aaudio/aaudio_device_manager.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace aqua::audio::aaudio {
namespace {

    // AudioStreamInfo 统一词汇与 AAudio 原生枚举数值锁定（诊断契约）。
    static_assert(AAUDIO_PERFORMANCE_MODE_NONE == AudioStreamInfo::kPerformanceNone,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");
    static_assert(AAUDIO_PERFORMANCE_MODE_LOW_LATENCY == AudioStreamInfo::kPerformanceLowLatency,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");
    static_assert(AAUDIO_PERFORMANCE_MODE_POWER_SAVING == AudioStreamInfo::kPerformancePowerSaving,
        "AAudio performance mode values must match AudioStreamInfo vocabulary");

    [[nodiscard]] std::string aaudio_result_name(aaudio_result_t result)
    {
        const char* name = AAudio_convertResultToText(result);
        return name != nullptr ? std::string(name) : std::string("unknown");
    }

    // core AudioEncoding -> aaudio_format_t。
    // U8 无对应（AAudio 无 U8 格式）→ 返回 false，上层直接 FormatUnsupported。
    [[nodiscard]] bool to_aaudio_format(AudioEncoding encoding, aaudio_format_t& out) noexcept
    {
        switch (encoding) {
        case AudioEncoding::PCM_S16LE:
            out = AAUDIO_FORMAT_PCM_I16;
            return true;
        case AudioEncoding::PCM_S24LE:
            out = AAUDIO_FORMAT_PCM_I24_PACKED;
            return true;
        case AudioEncoding::PCM_S32LE:
            out = AAUDIO_FORMAT_PCM_I32;
            return true;
        case AudioEncoding::PCM_F32LE:
            out = AAUDIO_FORMAT_PCM_FLOAT;
            return true;
        case AudioEncoding::PCM_U8:
        case AudioEncoding::INVALID:
            return false;
        }
        return false;
    }

    // 回读的 aaudio_format_t -> core AudioEncoding。
    [[nodiscard]] AudioEncoding from_aaudio_format(aaudio_format_t format) noexcept
    {
        switch (format) {
        case AAUDIO_FORMAT_PCM_I16:
            return AudioEncoding::PCM_S16LE;
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            return AudioEncoding::PCM_S24LE;
        case AAUDIO_FORMAT_PCM_I32:
            return AudioEncoding::PCM_S32LE;
        case AAUDIO_FORMAT_PCM_FLOAT:
            return AudioEncoding::PCM_F32LE;
        default:
            return AudioEncoding::INVALID;
        }
    }

    [[nodiscard]] AudioError map_aaudio_error(aaudio_result_t result) noexcept
    {
        switch (result) {
        case AAUDIO_ERROR_INVALID_FORMAT:
            return AudioError::FormatUnsupported;
        case AAUDIO_ERROR_DISCONNECTED:
            return AudioError::DeviceDisconnected;
        case AAUDIO_ERROR_INTERNAL:
        case AAUDIO_ERROR_UNAVAILABLE:
        case AAUDIO_ERROR_NO_FREE_HANDLES:
        case AAUDIO_ERROR_NO_MEMORY:
        case AAUDIO_ERROR_TIMEOUT:
            return AudioError::BackendFailed;
        default:
            return AudioError::BackendFailed;
        }
    }

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
    log_debug_fmt("AAudio playback config: device={} format={}ch/{}Hz/enc={} buffer_frames={} low_latency={}",
        config.device ? config.device->value() : std::string("default"),
        config.format.channels, config.format.sample_rate, static_cast<int>(config.format.encoding),
        config.frames_per_buffer, config.low_latency);

    if (running_.load(std::memory_order_acquire)) {
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

    aaudio_format_t requested_format = AAUDIO_FORMAT_INVALID;
    if (!to_aaudio_format(config.format.encoding, requested_format)) {
        log_error_fmt("AAudio playback: encoding {} has no AAudio representation",
            static_cast<int>(config.format.encoding));
        return std::unexpected(AudioError::FormatUnsupported);
    }

    // Android 路由策略：跟随系统（设计决议 §3）。resolve 仅用于设备方向
    // 校验与日志；显式 device id 在 Android 上不受支持。
    const auto resolved = device_manager_.resolve(AudioDeviceDirection::OUTPUT, config.device);
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

    // builder RAII：open 失败路径统一在这里 close。
    struct BuilderGuard {
        AAudioStreamBuilder* builder;
        ~BuilderGuard() { AAudioStreamBuilder_delete(builder); }
    } guard { raw_builder };

    // 契约格式全量下发；采样率是否被系统 SRC 由回读校验决定（设计决议 §1）。
    AAudioStreamBuilder_setFormat(raw_builder, requested_format);
    AAudioStreamBuilder_setChannelCount(raw_builder, static_cast<int32_t>(config.format.channels));
    AAudioStreamBuilder_setSampleRate(raw_builder, static_cast<int32_t>(config.format.sample_rate));
    AAudioStreamBuilder_setDirection(raw_builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(
        raw_builder,
        config.low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(raw_builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(raw_builder, AAUDIO_USAGE_MEDIA);
    // 播放设备路由（playback_switching_design.md §8）："android:N" 编码的
    // 显式设备映射为 setDeviceId；nullopt / 空 id 不设（跟随系统默认）。
    if (config.device.has_value() && !config.device->empty()) {
        const auto requested_device = parse_aaudio_device_id(*config.device);
        if (requested_device.has_value()) {
            AAudioStreamBuilder_setDeviceId(raw_builder, *requested_device);
            log_debug_fmt("AAudio playback: routing to explicit device id {}",
                *requested_device);
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
    AAudioStreamBuilder_setDataCallback(raw_builder, &AAudioAudioPlayback::on_data_callback, this);
    AAudioStreamBuilder_setErrorCallback(raw_builder, &AAudioAudioPlayback::on_error_callback, this);

    // 回调上下文：data callback 持有 shared_ptr 保活（stop 后在途回调仍安全）。
    try {
        callback_context_ = std::make_shared<CallbackContext>();
        callback_context_->callback = std::move(callback);
    } catch (...) {
        callback_context_.reset();
        return std::unexpected(AudioError::BackendFailed);
    }
    event_callback_ = std::move(event_callback);
    pending_error_.store(AudioError::None, std::memory_order_release);
    fatal_reported_.store(false, std::memory_order_release);

    AAudioStream* raw_stream = nullptr;
    result = AAudioStreamBuilder_openStream(raw_builder, &raw_stream);
    if (result != AAUDIO_OK || raw_stream == nullptr) {
        log_error_fmt("AAudio playback: openStream failed: {} ({})",
            aaudio_result_name(result), static_cast<int>(result));
        callback_context_.reset();
        event_callback_ = nullptr;
        return std::unexpected(map_aaudio_error(result));
    }

    // ---- 回读实际 stream 配置并做字节契约硬校验（设计决议 §1）----
    const auto actual_format = from_aaudio_format(AAudioStream_getFormat(raw_stream));
    const auto actual_channels = static_cast<std::uint32_t>(AAudioStream_getChannelCount(raw_stream));
    const auto actual_rate = static_cast<std::uint32_t>(AAudioStream_getSampleRate(raw_stream));

    if (actual_format != config.format.encoding) {
        log_error_fmt("AAudio playback: actual encoding {} != requested {} (rejected: byte-layout contract)",
            static_cast<int>(actual_format), static_cast<int>(config.format.encoding));
        AAudioStream_close(raw_stream);
        callback_context_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::FormatUnsupported);
    }
    if (actual_channels != config.format.channels) {
        log_error_fmt("AAudio playback: actual channels {} != requested {} (rejected: remix semantics uncontrolled)",
            actual_channels, config.format.channels);
        AAudioStream_close(raw_stream);
        callback_context_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::FormatUnsupported);
    }
    if (actual_rate != config.format.sample_rate) {
        // 采样率允许系统 SRC：JitterBuffer 水位机制吸收漂移（设计决议 §1.2）。
        log_info_fmt("AAudio playback: sample rate adjusted by system SRC: {} -> {} Hz",
            config.format.sample_rate, actual_rate);
    }

    callback_context_->frame_bytes = config.format.frame_bytes();
    if (callback_context_->frame_bytes == 0) {
        log_error("AAudio playback: frame_bytes resolved to 0");
        AAudioStream_close(raw_stream);
        callback_context_.reset();
        event_callback_ = nullptr;
        return std::unexpected(AudioError::InvalidArgument);
    }

    stream_ = raw_stream;

    result = AAudioStream_requestStart(raw_stream);
    if (result != AAUDIO_OK) {
        log_error_fmt("AAudio playback: requestStart failed: {} ({})",
            aaudio_result_name(result), static_cast<int>(result));
        AAudioStream_close(raw_stream);
        stream_ = nullptr;
        callback_context_.reset();
        event_callback_ = nullptr;
        return std::unexpected(map_aaudio_error(result));
    }

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

    if (stream_ != nullptr) {
        // requestStop 停止 data callback 调度；close 隐含 stop 并等待在途回调
        // 返回（AAudio 同步语义）。回调内不做任何 close——死锁约束由本控制
        // 线程独占执行（设计决议 §5）。
        (void)AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }

    // error callback 发布的 pending error（若有）在此投递：stop 路径的
    // event_callback_ 调用发生在控制线程，满足"不在回调线程内调 stop"契约。
    // 已即时投递过的错误（report_fatal_once）不重复投递，避免一次错误触发
    // 两次错误驱动恢复（多余的 stop/start 会拉长静音窗口）。
    const AudioError error = pending_error_.exchange(AudioError::None, std::memory_order_acq_rel);
    const bool already_reported = fatal_reported_.exchange(false, std::memory_order_acq_rel);
    if (error != AudioError::None && !already_reported) {
        log_debug_fmt("AAudio playback stopped with error: {}", audio_error_name(error));
        if (event_callback_) {
            try {
                event_callback_(error);
            } catch (...) {
                log_error("AAudio playback event callback exception");
            }
        }
    }

    callback_context_.reset();
    event_callback_ = nullptr;
    running_.store(false, std::memory_order_release);

    // 诊断缓存清零：stream_info() 回到 backend=None。
    info_sample_rate_.store(0, std::memory_order_relaxed);
    info_channels_.store(0, std::memory_order_relaxed);
    info_performance_mode_.store(0, std::memory_order_relaxed);
    info_frames_per_burst_.store(0, std::memory_order_relaxed);
    info_buffer_capacity_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(info_device_mutex_);
        info_device_id_.clear();
    }

    log_debug("AAudio playback stopped");
}

void AAudioAudioPlayback::publish_error(AudioError error) noexcept
{
    if (error == AudioError::None) {
        return;
    }
    pending_error_.store(error, std::memory_order_release);
}

void AAudioAudioPlayback::report_fatal_once(AudioError error) noexcept
{
    if (error == AudioError::None) {
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
    log_debug_fmt("AAudio playback: dispatching runtime error event: {}", audio_error_name(error));
    try {
        event_callback_(error);
    } catch (...) {
        log_error("AAudio playback event callback exception");
    }
}

aaudio_data_callback_result_t AAudioAudioPlayback::on_data_callback(
    AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) noexcept
{
    (void)stream; // AAudio 传递但不使用（pending_error_ 驱动停止）
    // user_data 是 start() 时传入的 this；回调仅在 stream 存活期间被调度，
    // this 生命周期覆盖（stop -> close 等待回调退出后才析构）。
    // 回调上下文经 shared_ptr 持有：即使 close 与本回调竞争，callback 对象
    // 依然保活（reset 不影响已持有的引用）。
    auto* self = static_cast<AAudioAudioPlayback*>(user_data);
    const auto context = self->callback_context_;
    if (context == nullptr || num_frames <= 0) {
        return AAUDIO_CALLBACK_RESULT_STOP;
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
            log_error("AAudio playback data callback exception");
            std::fill_n(static_cast<std::byte*>(audio_data), output_bytes, std::byte { 0 });
            self->report_fatal_once(AudioError::BackendFailed);
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
    }

    if (written_frames > static_cast<std::uint32_t>(num_frames)) {
        log_error_fmt("AAudio playback callback returned {} frames, but only {} requested",
            written_frames, num_frames);
        std::fill_n(static_cast<std::byte*>(audio_data), output_bytes, std::byte { 0 });
        self->report_fatal_once(AudioError::BackendFailed);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }

    // 契约：未填满部分补静音，避免复用残留数据（audio_playback.h 头注释）。
    const std::size_t written_bytes = static_cast<std::size_t>(written_frames) * context->frame_bytes;
    if (written_bytes < output_bytes) {
        std::fill(static_cast<std::byte*>(audio_data) + written_bytes,
            static_cast<std::byte*>(audio_data) + output_bytes,
            std::byte { 0 });
    }

    if (self->pending_error_.load(std::memory_order_acquire) != AudioError::None) {
        // error callback 已通过 report_fatal_once 即时上报；这里只停流。
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioAudioPlayback::on_error_callback(
    AAudioStream* stream, void* user_data, aaudio_result_t error) noexcept
{
    auto* self = static_cast<AAudioAudioPlayback*>(user_data);
    const AudioError mapped = map_aaudio_error(error);
    log_warn_fmt("AAudio playback error callback: {} ({}) -> {}",
        aaudio_result_name(error), static_cast<int>(error), audio_error_name(mapped));

    // 发布 pending error 并即时投递事件（不 close/stop，设计决议 §5）：
    // data callback 随后观察到 pending_error_ 自行返回 STOP；event 投递
    // 驱动 ClientRuntime 立即在 ioc 线程执行 restart 事务（与 WASAPI
    // 事件线程对等）。若只在 stop() 投递，流死后 runtime 无从感知——
    // 表现为 JB 被网络侧打满、输出永久静音。
    self->report_fatal_once(mapped);
    (void)stream;
}

} // namespace aqua::audio::aaudio
