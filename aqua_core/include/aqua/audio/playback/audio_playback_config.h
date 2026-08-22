#ifndef AQUA_AUDIO_PLAYBACK_CONFIG_H
#define AQUA_AUDIO_PLAYBACK_CONFIG_H

// 回放配置（值语义）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/devices/audio_device.h"

#include <cstdint>
#include <optional>

namespace aqua::audio {

struct AudioPlaybackConfig {
    // 设备选择：std::nullopt 表示系统默认输出设备；有值时使用指定输出设备 ID。
    std::optional<AudioDeviceId> device;

    // 回放格式：回调按该格式填充 output。设备不支持时 start() 返回 FormatUnsupported；
    // 回放不做转换，转换（如需）由上层在喂给回调之前完成。
    AudioFormat format;

    // 请求的缓冲大小（帧；所有声道合计的一组 sample frame）。
    // 0 表示由后端按设备/低延迟策略自行决定；后端实际回调粒度可以不同。
    std::uint32_t frames_per_buffer = 480;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_CONFIG_H
