#ifndef AQUA_WASAPI_PLAYBACK_H
#define AQUA_WASAPI_PLAYBACK_H

#include "core/audio/backend/audio_backend_factory.h"

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
    bool is_running() const override;

private:
    void playback_loop();

    std::thread thread_;
    // running_: 线程存活标志。start() 置 true；线程退出时（任何路径）置 false。
    //           stop() 也会置 false 以请求线程退出。
    std::atomic<bool> running_ { false };
    // started_: 初始化成功标志。仅当 playback_loop 内全部 WASAPI 初始化通过后置 true。
    //           start() 据此判断同步返回成功/失败。
    std::atomic<bool> started_ { false };
    FillCallback callback_;
    AudioFormat format_ { };
};

} // namespace aqua::audio

#endif // AQUA_WASAPI_PLAYBACK_H
