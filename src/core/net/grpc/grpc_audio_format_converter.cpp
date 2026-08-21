#include "core/net/grpc/grpc_audio_format_converter.h"
#include "core/audio/audio_config.h"

namespace aqua {

// 注意：参数名使用 proto_fmt 而非 pb，避免与命名空间 pb 遮蔽。
audio::AudioFormat from_proto(const pb::AudioFormat& proto_fmt)
{
audio:: AudioFormat fmt;
    switch (proto_fmt.encoding()) {
    case pb::AudioFormat::ENCODING_PCM_S16LE:
        fmt.encoding =audio:: AudioEncoding::PCM_S16LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S32LE:
        fmt.encoding =audio:: AudioEncoding::PCM_S32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_F32LE:
        fmt.encoding =audio:: AudioEncoding::PCM_F32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S24LE:
        fmt.encoding =audio:: AudioEncoding::PCM_S24LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_U8:
        fmt.encoding =audio:: AudioEncoding::PCM_U8;
        break;
    default:
        fmt.encoding =audio:: AudioEncoding::INVALID;
        break;
    }

    // proto3 的 int32 无范围约束：负数/超大值会经隐式转换变成巨大 uint32，
    // 通过 AudioFormat::valid() 后在下游（frame_bytes / WAVEFORMATEX 截断）造成
    // 除零或错误格式。此处显式校验。
    const std::uint32_t channels = proto_fmt.channels();
    const std::uint32_t sample_rate = proto_fmt.sample_rate();
    if (channels < 1 || channels > static_cast<int>(audio::AUDIO_MAX_CHANNELS) || sample_rate <= 0 || sample_rate > static_cast<int>(audio::AUDIO_MAX_SAMPLE_RATE)) {
        fmt.encoding =audio:: AudioEncoding::INVALID;
        fmt.channels = 0;
        fmt.sample_rate = 0;
        return fmt;
    }
    fmt.channels = static_cast<std::uint32_t>(channels);
    fmt.sample_rate = static_cast<std::uint32_t>(sample_rate);
    return fmt;
}

pb::AudioFormat to_proto(const audio::AudioFormat& fmt)
{
    pb::AudioFormat proto_fmt;
    switch (fmt.encoding) {
    case audio::AudioEncoding::PCM_S16LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S16LE);
        break;
    case audio::AudioEncoding::PCM_S32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S32LE);
        break;
    case audio::AudioEncoding::PCM_F32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_F32LE);
        break;
    case audio::AudioEncoding::PCM_S24LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S24LE);
        break;
    case audio::AudioEncoding::PCM_U8:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_U8);
        break;
    default:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_INVALID);
        break;
    }
    proto_fmt.set_channels(fmt.channels);
    proto_fmt.set_sample_rate(fmt.sample_rate);
    return proto_fmt;
}

} // namespace aqua
