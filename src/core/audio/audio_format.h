#ifndef AQUA_AUDIO_FORMAT_H
#define AQUA_AUDIO_FORMAT_H

#include <cstdint>
#include <limits>

#include "core/audio/audio_config.h"

namespace aqua::audio {

// 音频编码格式，与 aqua_service.proto 中 AudioFormat.Encoding 一一对应。
// 枚举值必须保持同步，禁止随意修改。
enum class AudioEncoding : std::uint8_t {
    INVALID = 0,
    PCM_S16LE = 1,
    PCM_S32LE = 2,
    PCM_F32LE = 3,
    PCM_S24LE = 4,
    PCM_U8 = 5,
};

// 原生 AudioFormat，仅描述音频数据本身。
// 不包含 frame_samples / packet_size / jitter_buffer_size 等传输层或播放策略字段。
// 用于音频管线内部，避免音频后端代码直接依赖 protobuf 生成类型。
struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::INVALID;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] bool is_valid() const noexcept
    {
        const auto bytes = bytes_per_sample();
        return bytes > 0
            && channels > 0
            && channels <= AUDIO_MAX_CHANNELS
            && channels <= std::numeric_limits<std::uint32_t>::max() / bytes
            && sample_rate > 0
            && sample_rate <= AUDIO_MAX_SAMPLE_RATE;
    }

    // 单个 sample（单声道）的字节数。
    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept
    {
        switch (encoding) {
        case AudioEncoding::PCM_S16LE:
            return 2;
        case AudioEncoding::PCM_S24LE:
            return 3;
        case AudioEncoding::PCM_S32LE:
            return 4;
        case AudioEncoding::PCM_F32LE:
            return 4;
        case AudioEncoding::PCM_U8:
            return 1;
        case AudioEncoding::INVALID:
            return 0;
        }
        return 0;
    }

    // 一帧（所有声道）的字节数。
    [[nodiscard]] std::uint32_t frame_bytes() const noexcept
    {
        const auto bytes = bytes_per_sample();
        if (bytes == 0 || channels > std::numeric_limits<std::uint32_t>::max() / bytes) {
            return 0;
        }
        return bytes * channels;
    }

    [[nodiscard]] bool operator==(const AudioFormat&) const noexcept = default;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_FORMAT_H
