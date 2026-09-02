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

std::expected<void, audio::AudioError> PlaybackManager::start(
    const audio::AudioPlaybackConfig& config,
    audio::AudioPlaybackCallback callback,
    audio::AudioPlaybackEventCallback event_callback) noexcept
{
    if (!playback_) {
        return std::unexpected(audio::AudioError::BackendFailed);
    }
    state_.store(PlaybackState::Starting, std::memory_order_release);
    const auto result = playback_->start(config, std::move(callback), std::move(event_callback));
    if (!result) {
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        return result;
    }
    state_.store(PlaybackState::Running, std::memory_order_release);
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
