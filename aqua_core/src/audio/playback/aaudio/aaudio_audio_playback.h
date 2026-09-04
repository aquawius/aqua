#ifndef AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H

// AAudio 回放后端（Android）。
//
// 设计决议见 aqua_core/doc/aaudio_backend_design.md：
//   - 格式协商：请求 server 契约格式；open 后回读实际配置，编码/声道必须
//     一致（字节布局硬约束），采样率允许系统 SRC（水位机制吸收漂移）；
//   - LOW_LATENCY/NONE + SHARED 由 AudioPlaybackConfig::low_latency 选择，
//     framesPerCallback 自适应（0 = 设备原生 burst）；
//   - data callback 内禁止 close（AAudio 死锁）：错误路径只返回
//     AAUDIO_CALLBACK_RESULT_STOP，close 由控制线程 stop() 执行；
//   - error callback 发布 pending error（原子，供 data callback 观察后 STOP）
//     并即时投递 event callback（与 WASAPI 事件线程对等）：运行期错误必须
//     立刻进入 ClientRuntime 的错误驱动恢复，否则流死后无人感知
//     （JB 打满、永久静音）。
//
// 线程模型：AAudio 内部 realtime 线程驱动 data callback（本类不创建音频
// 线程）；stop() 保证回调退出后才返回（AAudioStream_requestStop +
// AAudioStream_close 的同步语义）。

#include "aqua/audio/playback/audio_playback.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace aqua::audio::aaudio {

class AAudioAudioPlayback final : public AudioPlayback {
public:
    explicit AAudioAudioPlayback(AudioDeviceManager& device_manager);
    ~AAudioAudioPlayback() override;

    AAudioAudioPlayback(const AAudioAudioPlayback&) = delete;
    AAudioAudioPlayback& operator=(const AAudioAudioPlayback&) = delete;

    std::expected<void, AudioError> start(
        const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept override;

    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] AudioStreamInfo stream_info() const noexcept override;

    void stop() noexcept override;

private:
    // data callback 上下文：AAudio 回调线程持有 shared_ptr（stop() 后仍可能
    // 有一次在途回调，故不能捕获 this）。
    struct CallbackContext {
        AudioPlaybackCallback callback;
        // 帧几何（open 后回读，回调内只读）
        std::uint32_t frame_bytes = 0;
    };

    static aaudio_data_callback_result_t on_data_callback(
        AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) noexcept;

    static void on_error_callback(
        AAudioStream* stream, void* user_data, aaudio_result_t error) noexcept;

    void publish_error(AudioError error) noexcept;

    // 运行期致命错误的一次性上报（error callback 线程 / data callback 异常
    // 路径共用）：发布 pending error（data callback 观察后返回 STOP）、
    // 将 running_ 置 false（is_running 如实反映流已死）、并即时调用
    // event_callback_ 进入错误驱动恢复。fatal_reported_ 保证每次 start
    // 生命周期内只投递一次；stop() 据此跳过重复投递。
    void report_fatal_once(AudioError error) noexcept;

    AudioDeviceManager& device_manager_;

    AAudioStream* stream_ = nullptr;
    std::shared_ptr<CallbackContext> callback_context_;

    // 运行期事件回调。线程安全论证：写入只发生在 start()（openStream 之前，
    // 回调尚不可能触发）与 stop()（AAudioStream_close 已等待全部回调退出）
    // 两条控制路径；流存活期间无写者，error/data callback 线程可安全读取调用。
    AudioPlaybackEventCallback event_callback_;
    std::atomic<AudioError> pending_error_ { AudioError::None };
    // 本次 start 生命周期内致命错误是否已即时投递（stop() 据此去重）。
    std::atomic<bool> fatal_reported_ { false };

    std::atomic<bool> running_ { false };

    // stream_info() 原子缓存：start() 在控制线程写入（open 后回读一次），
    // stop() 清零；任意线程 relaxed 读，近似一致性满足诊断用途。
    std::atomic<std::uint32_t> info_sample_rate_ { 0 };
    std::atomic<std::uint32_t> info_channels_ { 0 };
    std::atomic<std::int32_t> info_performance_mode_ { 0 };
    std::atomic<std::uint32_t> info_frames_per_burst_ { 0 };
    std::atomic<std::uint32_t> info_buffer_capacity_ { 0 };
    // 运行期统计（data callback 线程写，stream_info() relaxed 读）：
    // callback 数每 data callback 递增；xrun 每 64 次回调采样一次
    // AAudioStream_getXRunCount（RT 上节流，避免每次回调做 native 调用）。
    std::atomic<std::uint64_t> stats_callback_count_ { 0 };
    std::atomic<std::uint64_t> stats_xrun_count_ { 0 };

    // 实际输出设备回读（"android:N"；playback_switching_design.md §8）：
    // 字符串不能原子化，mutex 保护诊断冷路径。
    mutable std::mutex info_device_mutex_;
    std::string info_device_id_;
};

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H
