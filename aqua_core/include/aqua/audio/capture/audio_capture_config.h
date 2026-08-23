#ifndef AQUA_AUDIO_CAPTURE_CONFIG_H
#define AQUA_AUDIO_CAPTURE_CONFIG_H

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device.h"

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
    AudioCaptureSource source = AudioCaptureSource::INPUT_DEVICE;

    // std::nullopt 表示该 source 对应方向的系统默认设备；有值时使用指定设备 ID。
    // INPUT_DEVICE -> INPUT endpoint；OUTPUT_LOOPBACK -> OUTPUT endpoint。
    std::optional<AudioDeviceId> device;

    // 请求的采集格式。std::nullopt 表示由 backend 使用该 stream 的默认/shared-mode 格式。
    // capture 不做格式转换；指定 format 时必须由 backend 原生支持，否则返回 FormatUnsupported。
    std::optional<AudioFormat> format;

    // 请求的缓冲大小（帧）；实际回调粒度由后端决定。0 表示由后端选择低延迟默认值。
    std::uint32_t frames_per_buffer = 480;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_CONFIG_H
