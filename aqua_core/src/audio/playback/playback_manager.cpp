#include "aqua/audio/playback/playback_manager.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"

#include <vector>

namespace aqua::audio {

PlaybackManager::PlaybackManager(AudioDeviceManager& device_manager)
    : playback_(create_playback(device_manager))
{
    if (!playback_) {
        log_error("PlaybackManager: audio playback backend is unavailable on this platform");
    }
}

PlaybackManager::PlaybackManager(std::unique_ptr<AudioPlayback> playback)
    : playback_(std::move(playback))
{
}

std::expected<void, AudioError> PlaybackManager::start_stream(
    const AudioPlaybackConfig& config,
    const std::shared_ptr<CallbackBundle>& bundle) noexcept
{
    // 包装转发：lambda 持有 bundle 的 shared_ptr 引用（AudioPlaybackCallback
    // 不可拷贝；包装后 restart 可重复传入同一回调）。
    auto wrapped_pull = [bundle](std::span<std::byte> output) noexcept {
        return bundle->pull(output);
    };
    AudioPlaybackEventCallback wrapped_event;
    if (bundle->event) {
        wrapped_event = [bundle](AudioError error) noexcept {
            bundle->event(error);
        };
    }
    return playback_->start(config, std::move(wrapped_pull), std::move(wrapped_event));
}

std::expected<void, AudioError> PlaybackManager::start(
    const AudioPlaybackConfig& config,
    AudioPlaybackCallback callback,
    AudioPlaybackEventCallback event_callback) noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callback) {
        // 后端只见到非空的包装回调，空回调校验收敛在 manager。
        return std::unexpected(AudioError::InvalidArgument);
    }

    auto bundle = std::make_shared<CallbackBundle>();
    bundle->pull = std::move(callback);
    bundle->event = std::move(event_callback);

    state_.store(PlaybackState::Starting, std::memory_order_release);
    const auto result = start_stream(config, bundle);
    if (!result) {
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        return result;
    }
    active_config_ = config;
    callbacks_ = std::move(bundle);
    cache_active_device(config.device);
    // 初始路由模式（playback_switching_design.md §4）：显式设备 ->
    // PreferredDevice；无显式设备时按连接起步设置——hold_current
    // （"自动切换"关）钉住首流实际设备，否则跟随系统。
    if (config.device) {
        route_mode_.store(
            PlaybackRouteMode::PreferredDevice, std::memory_order_release);
    } else if (hold_current_on_start_) {
        // 钉住实际设备：后续错误驱动 restart 锚定显式 id，不跟随新的
        // 系统默认。后端不提供回读（device_id 为空）时保持 nullopt，
        // 退化为 FollowSystem 的 restart 语义。
        const auto actual = playback_->stream_info().device_id;
        if (!actual.empty()) {
            active_config_.device = actual;
        }
        route_mode_.store(PlaybackRouteMode::HoldCurrent, std::memory_order_release);
    } else {
        route_mode_.store(
            PlaybackRouteMode::FollowSystem, std::memory_order_release);
    }
    state_.store(PlaybackState::Running, std::memory_order_release);
    return result;
}

std::expected<void, AudioError> PlaybackManager::restart() noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        // 尚未成功 start 过，没有"旧配置"可重启。
        return std::unexpected(AudioError::NotRunning);
    }

    state_.store(PlaybackState::Switching, std::memory_order_release);
    // break-before-make：stop() 同步 join 旧回调线程，返回后旧回调不再
    // 访问 JitterBuffer（AudioPlayback::stop 契约），JB 消费者唯一性在此交接。
    playback_->stop();
    const auto result = start_stream(active_config_, callbacks_);
    if (!result) {
        // A-0 语义：无 fallback 链，失败即停。
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        log_error_fmt("PlaybackManager restart failed: {}",
            audio_error_name(result.error()));
        return result;
    }
    cache_active_device(active_config_.device);
    state_.store(PlaybackState::Running, std::memory_order_release);
    log_debug("PlaybackManager restart completed: playback stream rebuilt");
    return result;
}

void PlaybackManager::cache_active_device(
    const std::optional<AudioDeviceId>& requested) noexcept
{
    // 成功 start 后立即落盘「实际输出设备」：优先 stream_info 回读（backend
    // open 后缓存的真实 endpoint/device），回读为空退回请求值。此后
    // previous_active_device 不再依赖 backend 的实时 stream_info 状态——
    // 设备 error 后 stream_info 可能尚未清零或已清零，都不影响切换/回滚决策。
    const auto info = stream_info();
    active_device_ = info.device_id.empty()
        ? requested
        : std::optional<AudioDeviceId> { info.device_id };
}

std::optional<AudioDeviceId> PlaybackManager::previous_active_device() const noexcept
{
    // 优先 PlaybackManager 缓存（生命周期状态），其次 backend 实时回读，
    // 最后退回请求值。三层兜底保证切换/恢复不因 backend 状态未及时更新而丢
    // 失「之前实际在哪个设备上」这一关键信息。
    if (active_device_.has_value()) {
        return active_device_;
    }
    const auto info = stream_info();
    if (!info.device_id.empty()) {
        return info.device_id;
    }
    return active_config_.device;
}

std::expected<SwitchResult, AudioError> PlaybackManager::switch_to(
    std::optional<AudioDeviceId> target) noexcept
{
    state_.store(PlaybackState::Switching, std::memory_order_release);

    // 捕获 previous_active_device（必须在 stop 前回读；stop 后缓存清零）。
    const auto previous = previous_active_device();

    // break-before-make：stop() 同步 join 旧回调线程。
    playback_->stop();

    // 候选链（playback_switching_design.md §5）：[target, previous,
    // system_default]，按 optional<AudioDeviceId> 相等去重（nullopt 与
    // nullopt 亦相等）。链固定三层，不做全设备遍历。
    std::vector<std::optional<AudioDeviceId>> candidates;
    const auto push_dedup = [&](std::optional<AudioDeviceId> candidate) {
        for (const auto& existing : candidates) {
            if (existing == candidate) {
                return;
            }
        }
        candidates.push_back(std::move(candidate));
    };
    push_dedup(target);
    push_dedup(previous);
    push_dedup(std::nullopt);

    AudioError last_error = AudioError::BackendFailed;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto cfg = active_config_;
        cfg.device = candidates[i];
        const auto result = start_stream(cfg, callbacks_);
        if (result.has_value()) {
            active_config_ = cfg;
            cache_active_device(candidates[i]);
            // 结果按成功候选的值判定（序号在去重后不可靠）：目标是
            // nullopt 且一次成功 = Switched；落在先前实际设备 = RolledBack；
            // 落系统默认（nullopt 兜底）= FellBackToSystem。
            const auto outcome = i == 0
                ? SwitchOutcome::Switched
                : (candidates[i] ? SwitchOutcome::RolledBack
                                 : SwitchOutcome::FellBackToSystem);
            const SwitchResult switch_result { outcome, AudioError::None };
            last_switch_result_.store(switch_result, std::memory_order_release);
            state_.store(PlaybackState::Running, std::memory_order_release);
            log_info_fmt(
                "PlaybackManager switch completed: outcome={} device={} (candidates={})",
                i == 0 ? "switched" : (i == 1 ? "rolled_back" : "fell_back_to_system"),
                candidates[i] ? candidates[i]->value() : std::string("system_default"),
                candidates.size());
            return switch_result;
        }
        last_error = result.error();
        log_warn_fmt("PlaybackManager switch candidate {} failed: {}",
            candidates[i] ? candidates[i]->value() : std::string("system_default"),
            audio_error_name(last_error));
    }

    // 链耗尽 = 格式不兼容（或重试超限后进入本路径）：Fatal 终态。
    const SwitchResult switch_result { SwitchOutcome::Fatal, last_error };
    last_switch_result_.store(switch_result, std::memory_order_release);
    state_.store(PlaybackState::Fatal, std::memory_order_release);
    log_error_fmt("PlaybackManager switch exhausted fallback chain: {}",
        audio_error_name(last_error));
    return std::unexpected(last_error);
}

std::expected<SwitchResult, AudioError> PlaybackManager::set_playback_device(
    std::optional<AudioDeviceId> target) noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        // Fatal 是终态：链耗尽后不再接受事务（supervision 将 stop runtime）。
        log_warn("PlaybackManager: set_playback_device rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }

    // 用户显式选择：不计数并重置重试窗口（防抖策略 §5）。
    error_restarts_in_window_ = 0;
    window_start_ = std::chrono::steady_clock::now();

    const auto result = switch_to(std::move(target));
    if (result.has_value()) {
        route_mode_.store(
            active_config_.device ? PlaybackRouteMode::PreferredDevice
                                  : PlaybackRouteMode::FollowSystem,
            std::memory_order_release);
    }
    return result;
}

std::expected<SwitchResult, AudioError> PlaybackManager::restart_on_error() noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        log_warn("PlaybackManager: restart_on_error rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }

    // 重试上限：10s 窗口最多 3 次，超限按链耗尽处理（防重启死循环）。
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start_ >= kRetryWindow) {
        error_restarts_in_window_ = 0;
        window_start_ = now;
    }
    if (error_restarts_in_window_ >= kMaxErrorRestarts) {
        const SwitchResult switch_result { SwitchOutcome::Fatal, AudioError::BackendFailed };
        last_switch_result_.store(switch_result, std::memory_order_release);
        state_.store(PlaybackState::Fatal, std::memory_order_release);
        log_error("PlaybackManager: error-driven restart retry budget exhausted");
        return std::unexpected(AudioError::BackendFailed);
    }
    ++error_restarts_in_window_;

    // 目标由路由模式推导（§4）：FollowSystem -> 系统默认；HoldCurrent ->
    // 之前的实际设备；PreferredDevice -> 当前请求设备。
    std::optional<AudioDeviceId> target;
    switch (route_mode_.load(std::memory_order_acquire)) {
    case PlaybackRouteMode::FollowSystem:
        target = std::nullopt;
        break;
    case PlaybackRouteMode::HoldCurrent:
        target = previous_active_device();
        break;
    case PlaybackRouteMode::PreferredDevice:
        target = active_config_.device;
        break;
    }

    // 不改变路由模式：fallback 是临时降级，用户意图不动。
    return switch_to(std::move(target));
}

void PlaybackManager::stop() noexcept
{
    if (playback_) {
        playback_->stop();
    }
    active_device_.reset();
    state_.store(PlaybackState::Inactive, std::memory_order_release);
}

} // namespace aqua::audio
