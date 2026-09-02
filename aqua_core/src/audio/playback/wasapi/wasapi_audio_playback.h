#ifndef AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H

#include "aqua/audio/playback/audio_playback.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace aqua::audio::wasapi {

class WasapiAudioPlayback final : public AudioPlayback {
public:
    explicit WasapiAudioPlayback(AudioDeviceManager& device_manager);
    ~WasapiAudioPlayback() override;

    WasapiAudioPlayback(const WasapiAudioPlayback&) = delete;
    WasapiAudioPlayback& operator=(const WasapiAudioPlayback&) = delete;

    std::expected<void, AudioError> start(
        const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept override;

    [[nodiscard]] bool is_running() const noexcept override;

    [[nodiscard]] AudioStreamInfo stream_info() const noexcept override;

    void stop() noexcept override;

private:
    struct StartState;

    void audio_thread_main(
        std::string device_id,
        AudioPlaybackConfig config,
        std::shared_ptr<StartState> start_state) noexcept;

    void audio_thread_main_impl(
        std::string device_id,
        AudioPlaybackConfig config,
        std::shared_ptr<StartState> start_state);

    void event_thread_main() noexcept;

    static void signal_start_state(
        const std::shared_ptr<StartState>& state,
        AudioError result) noexcept;

    AudioDeviceManager& device_manager_;

    std::thread audio_thread_;
    std::thread event_thread_;

    AudioPlaybackCallback frame_callback_;

    AudioPlaybackEventCallback event_callback_;

    std::atomic<bool> running_ = false;
    std::atomic<AudioError> pending_error_ = AudioError::None;

    // stream_info() 原子缓存：audio 线程在 start 完成前写入（Initialize/
    // GetBufferSize 后回读一次），stop() join 后清零；任意线程 relaxed 读。
    std::atomic<std::uint32_t> info_sample_rate_ { 0 };
    std::atomic<std::uint32_t> info_channels_ { 0 };
    std::atomic<std::int32_t> info_performance_mode_ { 0 };
    std::atomic<std::uint32_t> info_frames_per_burst_ { 0 };
    std::atomic<std::uint32_t> info_buffer_frames_ { 0 };

    // device_id 为字符串，不能原子化：以 mutex 保护（诊断冷路径，无实时影响）。
    // 写入 = audio 线程 start 末尾；清零 = stop() join 后；读 = stream_info()。
    mutable std::mutex info_device_mutex_;
    AudioDeviceId info_device_id_;

    void* stop_event_ = nullptr;
    void* audio_event_ = nullptr;
    void* error_event_ = nullptr;
};

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H
