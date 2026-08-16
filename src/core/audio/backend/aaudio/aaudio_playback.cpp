#include "core/audio/backend/aaudio/aaudio_playback.h"

#include "core/logger/logger.h"

#include <cstring>
#include <span>

namespace aqua::audio {

namespace {

// core AudioEncoding → AAudio 格式。不支持的返回 AAUDIO_FORMAT_UNSPECIFIED。
// 注意：AAudio 不支持 packed S24LE；S24/S32/U8 需客户端格式转换（AGENT.md §14）。
aaudio_format_t to_aaudio_format(AudioEncoding encoding) noexcept
{
    switch (encoding) {
    case AudioEncoding::PcmF32LE:
        return AAUDIO_FORMAT_PCM_FLOAT;
    case AudioEncoding::PcmS16LE:
        return AAUDIO_FORMAT_PCM_I16;
    default:
        return AAUDIO_FORMAT_UNSPECIFIED;
    }
}

} // namespace

AaudioPlayback::~AaudioPlayback()
{
    stop();
}

bool AaudioPlayback::start(AudioFormat format, FillCallback cb)
{
    if (stream_ != nullptr) {
        return false; // 已启动
    }
    if (!format.valid()) {
        return false;
    }

    const aaudio_format_t aaudio_fmt = to_aaudio_format(format.encoding);
    if (aaudio_fmt == AAUDIO_FORMAT_UNSPECIFIED) {
        log_error_fmt("AAudio playback: unsupported encoding {}",
                      static_cast<int>(format.encoding));
        return false;
    }

    frame_bytes_ = format.frame_bytes();
    callback_ = std::move(cb);

    aaudio_result_t res = AAudio_createStreamBuilder(&builder_);
    if (res != AAUDIO_OK || builder_ == nullptr) {
        log_error_fmt("AAudio playback: createStreamBuilder failed: {}",
                      AAudio_convertResultToText(res));
        builder_ = nullptr;
        callback_ = {};
        return false;
    }

    AAudioStreamBuilder_setDirection(builder_, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder_, aaudio_fmt);
    AAudioStreamBuilder_setChannelCount(builder_, static_cast<int32_t>(format.channels));
    AAudioStreamBuilder_setSampleRate(builder_, static_cast<int32_t>(format.sample_rate));
    AAudioStreamBuilder_setPerformanceMode(builder_, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder_, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setDataCallback(builder_, &AaudioPlayback::on_data_callback, this);
    AAudioStreamBuilder_setErrorCallback(builder_, &AaudioPlayback::on_error_callback, this);

    // openStream 阻塞直到打开成功或失败。
    res = AAudioStreamBuilder_openStream(builder_, &stream_);
    if (res != AAUDIO_OK || stream_ == nullptr) {
        log_error_fmt("AAudio playback: openStream failed: {}",
                      AAudio_convertResultToText(res));
        close_stream();
        callback_ = {};
        return false;
    }

    // 校验实际 stream 参数：open 可能协商出与请求不同的 format/rate/channels
    // （不假设 setFormat == 实际格式，见 AGENT.md Android 方案）。
    const aaudio_format_t actual_fmt = AAudioStream_getFormat(stream_);
    const int32_t actual_channels = AAudioStream_getChannelCount(stream_);
    const int32_t actual_rate = AAudioStream_getSampleRate(stream_);
    const int32_t frames_per_burst = AAudioStream_getFramesPerBurst(stream_);
    log_info_fmt("AAudio playback opened: fmt={} ch={} rate={} frames/burst={} "
                 "(requested fmt={} ch={} rate={})",
                 actual_fmt, actual_channels, actual_rate, frames_per_burst,
                 aaudio_fmt, format.channels, format.sample_rate);

    // requestStart（异步）；用 AAudio 内置阻塞等待进入 STARTED（非轮询 getState）。
    res = AAudioStream_requestStart(stream_);
    if (res != AAUDIO_OK) {
        log_error_fmt("AAudio playback: requestStart failed: {}",
                      AAudio_convertResultToText(res));
        close_stream();
        callback_ = {};
        return false;
    }

    constexpr int64_t kStartTimeoutNanos = 1'000'000'000; // 1s
    aaudio_stream_state_t state = AAudioStream_getState(stream_);
    if (state != AAUDIO_STREAM_STATE_STARTED) {
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        res = AAudioStream_waitForStateChange(stream_, state, &next, kStartTimeoutNanos);
        if (res != AAUDIO_OK) {
            log_error_fmt("AAudio playback: wait for STARTED failed: {}",
                          AAudio_convertResultToText(res));
            close_stream();
            callback_ = {};
            return false;
        }
        state = next;
    }

    if (state != AAUDIO_STREAM_STATE_STARTED) {
        log_error_fmt("AAudio playback: stream did not reach STARTED (state={})",
                      static_cast<int>(state));
        close_stream();
        callback_ = {};
        return false;
    }

    running_.store(true, std::memory_order_release);
    log_info("AAudio playback started");
    return true;
}

void AaudioPlayback::stop()
{
    running_.store(false, std::memory_order_release);
    close_stream();
    callback_ = {};
}

bool AaudioPlayback::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

aaudio_data_callback_result_t AaudioPlayback::on_data_callback(AAudioStream* /*stream*/,
                                                               void* user_data,
                                                               void* audio_data,
                                                               int32_t num_frames)
{
    auto* self = static_cast<AaudioPlayback*>(user_data);

    // 实时线程：只调 FillCallback（内部是 SPSC read，无锁）与补静音。
    // 严格禁止 mutex / malloc / 日志 / JNI / 网络。
    const std::size_t bytes_needed = static_cast<std::size_t>(num_frames) * self->frame_bytes_;
    std::size_t filled = 0;
    if (self->callback_) {
        filled = self->callback_(std::span<std::byte>{
            static_cast<std::byte*>(audio_data), bytes_needed});
    }
    // 防御：FillCallback 契约保证 filled <= bytes_needed，这里再钳制一次，
    // 避免异常返回值导致 memset 下溢（灾难性内存破坏）。
    if (filled > bytes_needed) {
        filled = bytes_needed;
    }
    if (filled < bytes_needed) {
        std::memset(static_cast<std::byte*>(audio_data) + filled, 0, bytes_needed - filled);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AaudioPlayback::on_error_callback(AAudioStream* /*stream*/,
                                       void* user_data,
                                       aaudio_result_t error)
{
    auto* self = static_cast<AaudioPlayback*>(user_data);
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        log_warn("AAudio playback: device disconnected");
    } else {
        log_warn_fmt("AAudio playback: error {}", static_cast<int>(error));
    }
    // 标记停止；client_runtime 主循环轮询 is_running() 感知后优雅关闭。
    self->running_.store(false, std::memory_order_release);
}

void AaudioPlayback::close_stream() noexcept
{
    if (stream_ != nullptr) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
    if (builder_ != nullptr) {
        AAudioStreamBuilder_delete(builder_);
        builder_ = nullptr;
    }
}

} // namespace aqua::audio
