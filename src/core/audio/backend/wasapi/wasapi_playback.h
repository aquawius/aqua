#ifndef AQUA_WASAPI_PLAYBACK_H
#define AQUA_WASAPI_PLAYBACK_H

#include "core/audio/backend/audio_backend.h"

#include <atomic>
#include <thread>

namespace aqua::audio {

// Windows WASAPI 播放后端。
// 以共享模式渲染到默认输出设备。
class WasapiPlayback : public PlaybackBackend {
public:
    WasapiPlayback() = default;
    ~WasapiPlayback() override;

    bool start(AudioFormat format, FillCallback cb) override;
    void stop() override;

private:
    void playback_loop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    FillCallback callback_;
    AudioFormat format_{};
};

} // namespace aqua::audio

#endif // AQUA_WASAPI_PLAYBACK_H
