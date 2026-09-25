#ifndef AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H
#define AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_route.h"

#include <cstdint>
#include <optional>

namespace aqua::audio {

enum class AudioCaptureSource {
    INPUT_DEVICE,
    OUTPUT_LOOPBACK,
};

struct AudioCaptureConfig {
    // 输入设备采集，或输出设备的系统混音（loopback）。
    // Loopback 不是一个独立 AudioDevice，而是一种 capture source。
    // Generic AudioCapture API default is input; ServerRuntime overrides this to OUTPUT_LOOPBACK
    // for Aqua product startup defaults.
    AudioCaptureSource source = AudioCaptureSource::INPUT_DEVICE;

    // 路由请求（唯一的设备选择入口）。server 侧的 RoutePolicy 把
    // Application 解释为严格钉住：选定的采集源丢失即 Fatal，绝不静默换源
    // （采集内容与用户预期不符且无从告知）。
    AudioRoute route = AudioRoute::follow_system();

    // 请求的采集格式。std::nullopt 表示由 backend 使用该 stream 的默认/shared-mode 格式。
    // capture 不做格式转换；指定 format 时必须由 backend 原生支持，否则返回 FormatUnsupported。
    std::optional<AudioFormat> format;

    // 请求的缓冲大小（帧）；实际回调粒度由后端决定。0 表示由后端选择低延迟默认值。
    std::uint32_t frames_per_buffer = 0;
};

} // namespace aqua::audio

#endif //  AQUA_AUDIO_CAPTURE_AUDIO_CAPTURE_CONFIG_H
