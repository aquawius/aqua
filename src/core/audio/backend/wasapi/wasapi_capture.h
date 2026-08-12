#ifndef AQUA_WASAPI_CAPTURE_H
#define AQUA_WASAPI_CAPTURE_H

#include "core/audio/backend/audio_backend.h"

#include <atomic>
#include <thread>

namespace aqua::audio {

// Windows WASAPI Loopback 采集后端。
// 采集系统默认输出设备的混音流（loopback），格式由设备 mix format 决定。
class WasapiCapture : public CaptureBackend {
public:
    WasapiCapture() = default;
    ~WasapiCapture() override;

    bool start(CaptureCallback cb, AudioFormat& out_format) override;
    void stop() override;

private:
    void capture_loop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    CaptureCallback callback_;
    AudioFormat format_{};
};

} // namespace aqua::audio

#endif // AQUA_WASAPI_CAPTURE_H
