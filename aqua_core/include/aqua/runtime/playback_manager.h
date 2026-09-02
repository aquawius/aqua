#ifndef AQUA_RUNTIME_PLAYBACK_MANAGER_H
#define AQUA_RUNTIME_PLAYBACK_MANAGER_H

// PlaybackManager：播放生命周期的管理边界（doc/playback_switching_design.md）。
//
// 层级：ClientRuntime --> PlaybackManager --> AudioPlayback --> AAudio/WASAPI
//
// 职责边界（防止切换策略下沉到 backend）：
//   - PlaybackManager：active device、switching 状态、stop/start 顺序、
//     rollback、诊断；
//   - AudioPlayback：打开设备、创建流、callback 生命周期、backend error 转换。
// AudioPlayback 不提供 switch_device 之类的策略 API；restart 事务（同设备或
// 换设备）全部在 PlaybackManager 内编排为 stop -> start 序列。
//
// 本阶段为纯包裹层：行为与 ClientRuntime 直接持有 AudioPlayback 完全一致。
// PlaybackState 转换随 start/stop 自动维护（Starting -> Running / Inactive）。

#include "aqua/audio/playback/audio_playback.h"
#include "aqua/runtime/playback_state.h"

#include <atomic>
#include <expected>
#include <memory>

namespace aqua::runtime {

class PlaybackManager final {
public:
    // 创建平台回放后端；平台不支持时 available() == false。
    explicit PlaybackManager(audio::AudioDeviceManager& device_manager);

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    // 启动回放：转发 AudioPlayback::start，并维护 PlaybackState
    // （Starting -> Running；失败回 Inactive）。
    std::expected<void, audio::AudioError>
    start(const audio::AudioPlaybackConfig& config,
        audio::AudioPlaybackCallback callback,
        audio::AudioPlaybackEventCallback event_callback = { }) noexcept;

    // 停止回放并等待回调线程退出（AudioPlayback::stop 契约：返回后
    // callback 不再被调用）。PlaybackState -> Inactive。
    void stop() noexcept;

    [[nodiscard]] bool available() const noexcept { return playback_ != nullptr; }

    [[nodiscard]] bool is_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

    [[nodiscard]] PlaybackState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    // 回读输出流实际运行参数（start 成功前 / stop 后 backend=None）。
    [[nodiscard]] audio::AudioStreamInfo stream_info() const noexcept
    {
        return playback_ != nullptr ? playback_->stream_info() : audio::AudioStreamInfo { };
    }

private:
    std::unique_ptr<audio::AudioPlayback> playback_;
    std::atomic<PlaybackState> state_ { PlaybackState::Inactive };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_PLAYBACK_MANAGER_H
