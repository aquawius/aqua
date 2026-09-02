#ifndef AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
#define AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H

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
// restart 事务链（playback_switching_design.md §5）：
//   Switching -> 捕获 previous_active_device（stream_info 回读）-> stop 旧流
//   （同步 join 回调线程，AudioPlayback::stop 契约保证返回后旧回调不再访问
//   JitterBuffer）-> 依次尝试去重候选 [target, previous, system_default]
//   -> 首个成功者 Running；链耗尽 -> Fatal（终态，supervision 将 stop runtime）。
//   全程不触碰 JitterBuffer / playhead / 诊断计数。
//
// 防抖与重试上限（§5）：错误驱动的自动 restart（restart_on_error）在 10s
// 窗口内最多 3 次，超过按链耗尽处理（防蓝牙连接风暴造成重启死循环）。
// 用户显式选择（set_playback_device）不计数并重置窗口。
//
// 线程约定：start/restart/set_playback_device/restart_on_error/stop 必须
// 由同一控制线程串行调用（与 ClientRuntime 生命周期路径一致）；
// 查询（state/stream_info/route_mode/last_switch_result）任意线程。

#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_route_mode.h"
#include "aqua/audio/playback/playback_state.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>

namespace aqua::audio {

// 切换结果：成功路径的降级信息（playback_switching_design.md §9）。
enum class SwitchOutcome : std::uint8_t {
    None, // 尚未发生切换事务（start 后的初始状态）
    Switched, // 目标设备（或同设备 restart）一次成功
    RolledBack, // 目标失败，回滚 previous_active_device 成功
    FellBackToSystem, // 目标与回滚均失败，落系统默认成功
    Fatal, // 候选链耗尽（格式不兼容 / 重试超限）
};

struct SwitchResult {
    SwitchOutcome outcome = SwitchOutcome::None;
    AudioError last_error = AudioError::None; // 链上最后一次失败原因
};

class PlaybackManager final {
public:
    // 创建平台回放后端；平台不支持时 available() == false。
    explicit PlaybackManager(AudioDeviceManager& device_manager);

    // 直接注入后端实例（测试用；生产路径走上面的工厂构造）。
    explicit PlaybackManager(std::unique_ptr<AudioPlayback> playback);

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    // 启动回放：转发 AudioPlayback::start，并维护 PlaybackState
    // （Starting -> Running；失败回 Inactive）。
    // 成功后记住 config 与回调（restart 复用）；回调经 shared bundle 保活，
    // AudioPlaybackCallback 不可拷贝，restart 需要重新传入同一回调。
    // 初始路由模式由 config.device 推导：nullopt -> FollowSystem，
    // 有值 -> PreferredDevice。
    std::expected<void, AudioError>
    start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept;

    // 同设备 restart（A-0 语义）：stop 旧流 -> 以同一 config 重新 start。
    // 前置：此前 start() 成功过（否则 NotRunning）。
    // 失败（无 fallback 链）：PlaybackState -> Inactive 并返回错误。
    std::expected<void, AudioError> restart() noexcept;

    // 显式切换目标设备（用户选择；nullopt = 跟随系统）。
    // 走完整候选链（target -> previous -> system_default 去重）；
    // 成功后更新路由模式（有 id -> PreferredDevice，nullopt -> FollowSystem）。
    // 用户显式选择不计数并重置重试窗口。
    // 链耗尽：PlaybackState -> Fatal 并返回链上最后一个错误。
    std::expected<SwitchResult, AudioError>
    set_playback_device(std::optional<AudioDeviceId> target) noexcept;

    // 错误驱动的自动 restart（设备拔出 / 流断开等）：
    // 按当前路由模式推导目标（FollowSystem -> nullopt；HoldCurrent ->
    // 当前实际设备；PreferredDevice -> 当前请求设备），走同一候选链。
    // 受重试上限约束（10s 窗口最多 3 次），超限直接 Fatal（不触碰后端）。
    // 不改变路由模式（fallback 是临时降级，用户意图不动）。
    std::expected<SwitchResult, AudioError> restart_on_error() noexcept;

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

    [[nodiscard]] PlaybackRouteMode route_mode() const noexcept
    {
        return route_mode_.load(std::memory_order_acquire);
    }

    // 当前请求设备（PreferredDevice 时有值；空 = 跟随系统）。诊断用。
    [[nodiscard]] std::optional<AudioDeviceId> requested_device() const noexcept
    {
        return active_config_.device;
    }

    // 最近一次切换事务的结果（start 后为 Switched/None；从未切换 = nullopt）。
    [[nodiscard]] std::optional<SwitchResult> last_switch_result() const noexcept
    {
        return last_switch_result_.load(std::memory_order_acquire);
    }

    // 回读输出流实际运行参数（start 成功前 / stop 后 backend=None）。
    [[nodiscard]] AudioStreamInfo stream_info() const noexcept
    {
        return playback_ != nullptr ? playback_->stream_info() : AudioStreamInfo { };
    }

private:
    // 回调持有：start() 传入的回调存放于此，restart 复用。
    // MoveOnlyFunction 不可拷贝，故以 shared_ptr 保活并包装转发。
    struct CallbackBundle {
        AudioPlaybackCallback pull;
        AudioPlaybackEventCallback event;
    };

    // 以 bundle 包装回调并转发给后端（start/restart 共用）。
    std::expected<void, AudioError>
    start_stream(const AudioPlaybackConfig& config,
        const std::shared_ptr<CallbackBundle>& bundle) noexcept;

    // 完整切换事务（set_playback_device / restart_on_error 共用核心）：
    // 前置已检查；负责候选链去重、逐项尝试、状态与结果维护。
    std::expected<SwitchResult, AudioError>
    switch_to(std::optional<AudioDeviceId> target) noexcept;

    // previous_active_device：优先 stream_info 的实际设备回读，
    // 回读为空时退回 active_config_.device（请求值）。
    [[nodiscard]] std::optional<AudioDeviceId> previous_active_device() const noexcept;

    std::unique_ptr<AudioPlayback> playback_;
    std::shared_ptr<CallbackBundle> callbacks_;
    AudioPlaybackConfig active_config_ { };
    std::atomic<PlaybackState> state_ { PlaybackState::Inactive };
    std::atomic<PlaybackRouteMode> route_mode_ { PlaybackRouteMode::FollowSystem };
    std::atomic<SwitchResult> last_switch_result_ { };

    // 重试窗口（仅控制线程访问，与生命周期方法同线程串行）：
    // 错误驱动 restart 在窗口内最多 kMaxErrorRestarts 次。
    static constexpr auto kRetryWindow = std::chrono::seconds(10);
    static constexpr unsigned kMaxErrorRestarts = 3;
    std::chrono::steady_clock::time_point window_start_
        = std::chrono::steady_clock::now();
    unsigned error_restarts_in_window_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
