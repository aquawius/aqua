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

// ---- Event-starvation fallback 参数 ----
// loopback endpoint 在最后一个 render client 退出后可能进入 quiescence,audio event
// 不再触发。用有界等待替代 INFINITE:
//   - 超时后经正常 drain 路径主动探测 GetNextPacketSize;
//   - engine 仍有数据(event 丢失/迟到)→ 正常排空;
//   - engine 无数据 → 按墙钟时长合成静音 AudioBlock,使 capture 时间轴以 1x 速率推进。
// 20ms ≈ 2×10ms shared-mode engine period,避免对正常抖动过敏。
constexpr DWORD kCaptureEventTimeoutMs = 20;
// 连续 2 次超时(约 40ms)才把诊断状态标为 starved;合成静音从第一次超时就开始。
constexpr std::uint32_t kStarvedDeclareThreshold = 2;
// 单次合成静音块上限:防止调度延迟/系统挂起恢复后产生突发。
constexpr std::uint32_t kSynthSilenceMaxMs = 150;

class WasapiAudioCapture final : public AudioCapture {
public:
    explicit WasapiAudioCapture(AudioDeviceManager& device_manager);
    ~WasapiAudioCapture() override;

    WasapiAudioCapture(const WasapiAudioCapture&) = delete;
    WasapiAudioCapture& operator=(const WasapiAudioCapture&) = delete;

    std::expected<void, AudioError> start(
        const AudioCaptureConfig& config,
        AudioCaptureCallback frame_callback,
        AudioCaptureEventCallback event_callback) noexcept override;

    [[nodiscard]] const AudioCaptureInfo& info() const noexcept override;
    [[nodiscard]] bool is_running() const noexcept override;
    [[nodiscard]] AudioCaptureStats stats() const noexcept override;
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

    std::atomic<std::uint64_t> audio_events_ { 0 };
    std::atomic<std::uint64_t> packet_queries_ { 0 };
    std::atomic<std::uint64_t> packet_empty_ { 0 };
    std::atomic<std::uint64_t> packets_ready_ { 0 };
    std::atomic<std::uint64_t> get_buffer_success_ { 0 };
    std::atomic<std::uint64_t> callbacks_ { 0 };
    std::atomic<std::uint64_t> silent_callbacks_ { 0 };
    // 事件饥饿 fallback 统计（仅音频线程写，stats() 跨线程读，relaxed）。
    std::atomic<std::uint64_t> synthetic_silence_blocks_ { 0 };
    std::atomic<std::uint64_t> generated_silence_frames_ { 0 };
    std::atomic<std::uint64_t> starved_events_ { 0 };
    std::atomic<std::uint64_t> starved_ms_ { 0 };
    std::atomic<AudioCaptureState> capture_state_ { AudioCaptureState::Active };

    AudioCaptureCallback frame_callback_;
    AudioCaptureEventCallback event_callback_;

    std::thread audio_thread_;
    std::thread event_thread_;
};

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_WASAPI_AUDIO_CAPTURE_H
