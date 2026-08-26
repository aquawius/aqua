#ifndef AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H
#define AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H

#include "aqua/audio/playback/audio_playback.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

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
        AudioPlaybackEventCallback event_callback = {}) noexcept override;

    [[nodiscard]] bool is_running() noexcept override;

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

    void* stop_event_ = nullptr;
    void* audio_event_ = nullptr;
    void* error_event_ = nullptr;
};

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PLAYBACK_WASAPI_AUDIO_PLAYBACK_H
