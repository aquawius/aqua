#ifndef AQUA_AUDIO_CAPTURE_CONFIG_H
#define AQUA_AUDIO_CAPTURE_CONFIG_H

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device.h"

#include <cstdint>
#include <optional>

namespace aqua::audio {

struct AudioCaptureConfig {
    // 采集方向：INPUT = 麦克风等输入设备；OUTPUT = 输出设备的系统混音（loopback）。
    // 无 loopback 的平台（如 Android）对 OUTPUT 返回 NotSupported。
    AudioDeviceDirection direction = AudioDeviceDirection::INPUT;

    // 设备选择：std::nullopt 表示该方向的系统默认设备；有值时使用指定设备 ID。
    // 指定设备的 direction 必须与上面的 direction 一致，否则 start() 返回 DeviceNotFound。
    std::optional<AudioDeviceId> device;

    // 固定采集格式：capture 不做任何转换，交付严格等于该格式的 PCM。
    // 设备原生不支持该格式时 start() 返回 FormatUnsupported；转换（如需）由客户端完成。
    AudioFormat format;

    // 请求的缓冲大小（帧）；实际回调粒度由后端决定。0 表示由后端选择低延迟默认值。
    std::uint32_t frames_per_buffer = 480;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_CONFIG_H
