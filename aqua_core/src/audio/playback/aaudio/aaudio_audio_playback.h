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
//   - error callback 只发布 pending error（原子），不关流不停流。
//
// 线程模型：AAudio 内部 realtime 线程驱动 data callback（本类不创建音频
// 线程）；stop() 保证回调退出后才返回（AAudioStream_requestStop +
// AAudioStream_close 的同步语义）。

#include "aqua/audio/playback/audio_playback.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <memory>

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

    AudioDeviceManager& device_manager_;

    AAudioStream* stream_ = nullptr;
    std::shared_ptr<CallbackContext> callback_context_;

    // 运行期事件回调：由 error callback 线程外的控制路径投递（见实现注释）。
    AudioPlaybackEventCallback event_callback_;
    std::atomic<AudioError> pending_error_ { AudioError::None };

    std::atomic<bool> running_ { false };
};

} // namespace aqua::audio::aaudio

#endif // AQUA_AUDIO_PLAYBACK_AAUDIO_AUDIO_PLAYBACK_H
