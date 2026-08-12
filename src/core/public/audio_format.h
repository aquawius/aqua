#ifndef AQUA_SHARE_AUDIO_FORMAT_H
#define AQUA_SHARE_AUDIO_FORMAT_H

#include <cstdint>

namespace aqua {

// 音频编码格式，与 aqua_service.proto 中 AudioFormat.Encoding 一一对应。
// 枚举值必须保持同步，禁止随意修改。
enum class AudioEncoding : std::uint8_t {
    Invalid = 0,
    PcmS16LE = 1,
    PcmS32LE = 2,
    PcmF32LE = 3,
    PcmS24LE = 4,
    PcmU8 = 5,
};

// 原生 AudioFormat，仅描述音频数据本身。
// 不包含 frame_samples / packet_size / jitter_buffer_size 等传输层或播放策略字段。
// 用于音频管线内部，避免音频后端代码直接依赖 protobuf 生成类型。
struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::Invalid;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return encoding != AudioEncoding::Invalid && channels > 0 && sample_rate > 0;
    }

    // 单个 sample（单声道）的字节数。
    [[nodiscard]] std::uint32_t bytes_per_sample() const noexcept
    {
        switch (encoding) {
        case AudioEncoding::PcmS16LE:
            return 2;
        case AudioEncoding::PcmS24LE:
            return 3;
        case AudioEncoding::PcmS32LE:
            return 4;
        case AudioEncoding::PcmF32LE:
            return 4;
        case AudioEncoding::PcmU8:
            return 1;
        case AudioEncoding::Invalid:
            return 0;
        }
        return 0;
    }

    // 一帧（所有声道）的字节数。
    [[nodiscard]] std::uint32_t frame_bytes() const noexcept
    {
        return bytes_per_sample() * channels;
    }

    [[nodiscard]] bool operator==(const AudioFormat&) const noexcept = default;
};

} // namespace aqua

#endif // AQUA_SHARE_AUDIO_FORMAT_H
