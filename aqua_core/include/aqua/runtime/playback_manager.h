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
// restart 事务（Phase A-0：同设备、同配置）：
//   Switching -> stop()（同步 join 回调线程，AudioPlayback::stop 契约保证
//   返回后旧回调不再访问 JitterBuffer）-> start(旧 config) -> Running。
//   全程不触碰 JitterBuffer / playhead / 诊断计数（playback_switching_design.md
//   §5：JB 不清空、play_seq 不重置）。A-1 再引入设备目标、fallback 链与 Fatal。
//
// 线程约定：start/restart/stop 必须由同一控制线程串行调用（与
// ClientRuntime 生命周期路径一致）；查询（state/stream_info）任意线程。

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

    // 直接注入后端实例（测试用；生产路径走上面的工厂构造）。
    explicit PlaybackManager(std::unique_ptr<audio::AudioPlayback> playback);

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    // 启动回放：转发 AudioPlayback::start，并维护 PlaybackState
    // （Starting -> Running；失败回 Inactive）。
    // 成功后记住 config 与回调（restart 复用）；回调经 shared bundle 保活，
    // AudioPlaybackCallback 不可拷贝，restart 需要重新传入同一回调。
    std::expected<void, audio::AudioError>
    start(const audio::AudioPlaybackConfig& config,
        audio::AudioPlaybackCallback callback,
        audio::AudioPlaybackEventCallback event_callback = { }) noexcept;

    // A-0 restart 事务：stop 旧流 -> 以同一 config 重新 start。
    // 前置：此前 start() 成功过（否则 NotRunning）。
    // 失败（A-0 无 fallback 链）：PlaybackState -> Inactive 并返回错误。
    std::expected<void, audio::AudioError> restart() noexcept;

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
    // 回调持有：start() 传入的回调存放于此，restart() 复用。
    // MoveOnlyFunction 不可拷贝，故以 shared_ptr 保活并包装转发。
    struct CallbackBundle {
        audio::AudioPlaybackCallback pull;
        audio::AudioPlaybackEventCallback event;
    };

    // 以 bundle 包装回调并转发给后端（start/restart 共用）。
    std::expected<void, audio::AudioError>
    start_stream(const audio::AudioPlaybackConfig& config,
        const std::shared_ptr<CallbackBundle>& bundle) noexcept;

    std::unique_ptr<audio::AudioPlayback> playback_;
    std::shared_ptr<CallbackBundle> callbacks_;
    audio::AudioPlaybackConfig active_config_ { };
    std::atomic<PlaybackState> state_ { PlaybackState::Inactive };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_PLAYBACK_MANAGER_H
