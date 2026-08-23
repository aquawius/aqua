#ifndef AQUA_AUDIO_WASAPI_AUDIO_CAPTURE_H
#define AQUA_AUDIO_WASAPI_AUDIO_CAPTURE_H

#include "aqua/audio/capture/audio_capture.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace aqua::audio::wasapi {

class WasapiAudioCapture final : public AudioCapture {
public:
    explicit WasapiAudioCapture(AudioDeviceManager& device_manager);
    ~WasapiAudioCapture() override;

    WasapiAudioCapture(const WasapiAudioCapture&) = delete;
    WasapiAudioCapture& operator=(const WasapiAudioCapture&) = delete;

    std::expected<void, AudioError> start(
        const AudioCaptureConfig& config,
        AudioCaptureCallback frame_callback,
        void* frame_user_data,
        AudioCaptureEventCallback event_callback = nullptr,
        void* event_user_data = nullptr) noexcept override;

    [[nodiscard]] const AudioCaptureInfo& info() const noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    void stop() noexcept override;

private:
    struct StartState;

    static void signal_start_state(const std::shared_ptr<StartState>& state, AudioError result) noexcept;

    void audio_thread_main(
        std::string device_id,
        AudioCaptureConfig config,
        std::shared_ptr<StartState> start_state) noexcept;

    void audio_thread_main_impl(
        std::string device_id,
        AudioCaptureConfig config,
        std::shared_ptr<StartState> start_state);

    void event_thread_main() noexcept;

#ifdef _WIN32
    HANDLE stop_event_ = nullptr;
    HANDLE audio_event_ = nullptr;
    HANDLE error_event_ = nullptr;
#endif

    AudioDeviceManager& device_manager_;
    AudioCaptureInfo info_;

    std::atomic<bool> running_ { false };
    std::atomic<AudioError> pending_error_ { AudioError::None };

    AudioCaptureCallback frame_callback_ = nullptr;
    void* frame_user_data_ = nullptr;
    AudioCaptureEventCallback event_callback_ = nullptr;
    void* event_user_data_ = nullptr;

    std::thread audio_thread_;
    std::thread event_thread_;
};

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_WASAPI_AUDIO_CAPTURE_H
