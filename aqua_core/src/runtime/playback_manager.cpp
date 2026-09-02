#include "aqua/runtime/playback_manager.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"

namespace aqua::runtime {

PlaybackManager::PlaybackManager(audio::AudioDeviceManager& device_manager)
    : playback_(audio::create_playback(device_manager))
{
    if (!playback_) {
        log_error("PlaybackManager: audio playback backend is unavailable on this platform");
    }
}

PlaybackManager::PlaybackManager(std::unique_ptr<audio::AudioPlayback> playback)
    : playback_(std::move(playback))
{
}

std::expected<void, audio::AudioError> PlaybackManager::start_stream(
    const audio::AudioPlaybackConfig& config,
    const std::shared_ptr<CallbackBundle>& bundle) noexcept
{
    // 包装转发：lambda 持有 bundle 的 shared_ptr 引用（AudioPlaybackCallback
    // 不可拷贝；包装后 restart 可重复传入同一回调）。
    auto wrapped_pull = [bundle](std::span<std::byte> output) noexcept {
        return bundle->pull(output);
    };
    audio::AudioPlaybackEventCallback wrapped_event;
    if (bundle->event) {
        wrapped_event = [bundle](audio::AudioError error) noexcept {
            bundle->event(error);
        };
    }
    return playback_->start(config, std::move(wrapped_pull), std::move(wrapped_event));
}

std::expected<void, audio::AudioError> PlaybackManager::start(
    const audio::AudioPlaybackConfig& config,
    audio::AudioPlaybackCallback callback,
    audio::AudioPlaybackEventCallback event_callback) noexcept
{
    if (!playback_) {
        return std::unexpected(audio::AudioError::BackendFailed);
    }
    if (!callback) {
        // 后端只见到非空的包装回调，空回调校验收敛在 manager。
        return std::unexpected(audio::AudioError::InvalidArgument);
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
    state_.store(PlaybackState::Running, std::memory_order_release);
    return result;
}

std::expected<void, audio::AudioError> PlaybackManager::restart() noexcept
{
    if (!playback_) {
        return std::unexpected(audio::AudioError::BackendFailed);
    }
    if (!callbacks_) {
        // 尚未成功 start 过，没有"旧配置"可重启。
        return std::unexpected(audio::AudioError::NotRunning);
    }

    state_.store(PlaybackState::Switching, std::memory_order_release);
    // break-before-make：stop() 同步 join 旧回调线程，返回后旧回调不再
    // 访问 JitterBuffer（AudioPlayback::stop 契约），JB 消费者唯一性在此交接。
    playback_->stop();
    const auto result = start_stream(active_config_, callbacks_);
    if (!result) {
        // A-0 无 fallback 链：失败即停（A-1 引入三元链与 Fatal 终态）。
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        log_error_fmt("PlaybackManager restart failed: {}",
            audio::audio_error_name(result.error()));
        return result;
    }
    state_.store(PlaybackState::Running, std::memory_order_release);
    log_debug("PlaybackManager restart completed: playback stream rebuilt");
    return result;
}

void PlaybackManager::stop() noexcept
{
    if (playback_) {
        playback_->stop();
    }
    state_.store(PlaybackState::Inactive, std::memory_order_release);
}

} // namespace aqua::runtime
