//
// Created by aquawius on 25-4-12.
//

#ifndef AUDIO_FORMAT_COMMON_H
#define AUDIO_FORMAT_COMMON_H

#include "audio_service.grpc.pb.h"

namespace audio_common {
// 音频编码格式枚举
enum class AudioEncoding {
    INVALID = 0,
    PCM_S16LE = 1,
    PCM_S32LE = 2,
    PCM_F32LE = 3,
    PCM_S24LE = 4,
    PCM_U8 = 5
};

// 辅助函数：将audio_manager中的AudioEncoding转换为protobuf中的编码类型
static AudioService::auqa::pb::AudioFormat_Encoding convert_encoding_to_proto(AudioEncoding encoding)
{
    using ProtoEncoding = AudioService::auqa::pb::AudioFormat_Encoding;

    switch (encoding) {
    case AudioEncoding::PCM_S16LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S16LE;
    case AudioEncoding::PCM_S32LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S32LE;
    case AudioEncoding::PCM_F32LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_F32LE;
    case AudioEncoding::PCM_S24LE:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_S24LE;
    case AudioEncoding::PCM_U8:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_PCM_U8;
    case AudioEncoding::INVALID:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_INVALID;
    default:
        return ProtoEncoding::AudioFormat_Encoding_ENCODING_INVALID;
    }
}

// 辅助函数：将protobuf中的编码类型转换为audio_manager中的AudioEncoding
static AudioEncoding convert_proto_to_encoding(AudioService::auqa::pb::AudioFormat_Encoding encoding)
{
    using Encoding = AudioEncoding;

    switch (encoding) {
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S16LE:
        return Encoding::PCM_S16LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S32LE:
        return Encoding::PCM_S32LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_F32LE:
        return Encoding::PCM_F32LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_S24LE:
        return Encoding::PCM_S24LE;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_PCM_U8:
        return Encoding::PCM_U8;
    case AudioService::auqa::pb::AudioFormat_Encoding_ENCODING_INVALID:
        return Encoding::INVALID;
    default:
        return Encoding::INVALID;
    }
}

// 音频格式结构体
struct AudioFormat {
public:
    AudioEncoding encoding;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t bit_depth;

    bool operator==(const AudioFormat& other) const
    {
        return encoding == other.encoding &&
            channels == other.channels &&
            sample_rate == other.sample_rate &&
            bit_depth == other.bit_depth;
    }

    bool operator!=(const AudioFormat& other) const
    {
        return !(*this == other);
    }

    AudioFormat(): encoding(AudioEncoding::INVALID), channels(0), sample_rate(0), bit_depth(0) {}

    explicit AudioFormat(AudioEncoding encoding, uint32_t channals, uint32_t sample_rate)
        : encoding(encoding),
          channels(channals),
          sample_rate(sample_rate),
          bit_depth(get_bit_depth_from_encoding(encoding)) {}

    explicit AudioFormat(const AudioService::auqa::pb::AudioFormat& audio_format)
        : encoding(convert_proto_to_encoding(audio_format.encoding())),
          channels(audio_format.channels()),
          sample_rate(audio_format.sample_rate())
    {
        bit_depth = get_bit_depth_from_encoding(encoding);
    }

    static uint32_t get_bit_depth_from_encoding(const AudioEncoding encoding)
    {
        switch (encoding) {
        case AudioEncoding::PCM_S16LE:
            return 16;
        case AudioEncoding::PCM_S32LE:
            return 32;
        case AudioEncoding::PCM_F32LE:
            return 32;
        case AudioEncoding::PCM_S24LE:
            return 24;
        case AudioEncoding::PCM_U8:
            return 8;
        case AudioEncoding::INVALID:
            return 0;

        default:
            return 0;
        }
    }

    static std::optional<bool> is_float_encoding(const AudioEncoding encoding)
    {
        switch (encoding) {
        case AudioEncoding::PCM_F32LE:
            return true;
        case AudioEncoding::PCM_S16LE:
            [[fallthrough]];
        case AudioEncoding::PCM_S32LE:
            [[fallthrough]];
        case AudioEncoding::PCM_S24LE:
            [[fallthrough]];
        case AudioEncoding::PCM_U8:
            return false;
        case AudioEncoding::INVALID:
            return std::nullopt; // 明确表示无效编码
        default:
            return std::nullopt;
        }
    }

    static constexpr bool is_valid(const AudioFormat format)
    {
        return is_valid_encoding(format.encoding) &&
            is_valid_channels(format.channels) &&
            is_valid_sample_rate(format.sample_rate) &&
            is_valid_bit_depth(format.bit_depth, format.encoding);
    }

private:
    // 编码有效性检查
    static constexpr bool is_valid_encoding(const AudioEncoding encoding)
    {
        switch (encoding) {
        case AudioEncoding::PCM_S16LE:
        case AudioEncoding::PCM_S32LE:
        case AudioEncoding::PCM_F32LE:
        case AudioEncoding::PCM_S24LE:
        case AudioEncoding::PCM_U8:
            return true;
        case AudioEncoding::INVALID:
        default: // 处理未知枚举值
            return false;
        }
    }

    // 通道数检查（支持单声道到7.1环绕声）
    static bool is_valid_channels(const uint32_t channels)
    {
        return channels >= 1 && channels <= 8; // 根据实际需求调整上限
    }

    // 采样率检查（覆盖常见音频范围）
    static bool is_valid_sample_rate(const uint32_t sample_rate)
    {
        return sample_rate >= 8000 && sample_rate <= 384000; // 包含电话级8K到高解析384K
    }

    // 位深度一致性检查
    static bool is_valid_bit_depth(const uint32_t bit_depth, const AudioEncoding encoding)
    {
        // 确保bit_depth与编码标准匹配
        const uint32_t expected = get_bit_depth_from_encoding(encoding);
        return bit_depth == expected && expected != 0; // 排除INVALID编码
    }

};
} // namespace audio_common

#endif // AUDIO_FORMAT_COMMON_H