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

// ---- 欠账驱动的时间轴补偿参数 ----
// loopback endpoint 在最后一个 render client 退出后可能进入 quiescence，audio event
// 不再触发；切歌等 render 流重建期间 engine 也可能反复 signal event 但不产出 packet。
// 采集时间轴必须与 engine 的 event 行为解耦、恒以 1x 墙钟速率推进：
//   - 每轮唤醒（事件或超时）统一对账：expected = 墙钟欠账（含小数累积），
//     与本轮真实交付帧数对差：欠账（balance>0）立即合成静音补齐；
//     盈余（balance<0，engine 暴发）留存抵扣未来欠账。
//   - 空事件、零星小包（部分饥饿）、完全静默三种情形由同一公式覆盖。
// 20ms ≈ 2×10ms shared-mode engine period，是唤醒/探测粒度（避免对正常抖动过敏）。
constexpr DWORD kCaptureEventTimeoutMs = 20;
// 连续 2 轮补偿（约 40ms 欠账）才把诊断状态标为 starved；合成静音从第一轮欠账就开始。
constexpr std::uint32_t kStarvedDeclareThreshold = 2;
// 单轮补偿的静音帧上限，同时是盈余留存上限：防止调度延迟/系统挂起恢复后产生突发，
// 超出上限的欠账丢弃（不追历史）。
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
