#include "core/grpc/audio_format_converter.h"

namespace aqua {

// 注意：参数名使用 proto_fmt 而非 pb，避免与命名空间 pb 遮蔽。
AudioFormat from_proto(const pb::AudioFormat& proto_fmt)
{
    AudioFormat fmt;
    switch (proto_fmt.encoding()) {
    case pb::AudioFormat::ENCODING_PCM_S16LE:
        fmt.encoding = AudioEncoding::PcmS16LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S32LE:
        fmt.encoding = AudioEncoding::PcmS32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_F32LE:
        fmt.encoding = AudioEncoding::PcmF32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S24LE:
        fmt.encoding = AudioEncoding::PcmS24LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_U8:
        fmt.encoding = AudioEncoding::PcmU8;
        break;
    default:
        fmt.encoding = AudioEncoding::Invalid;
        break;
    }
    fmt.channels = proto_fmt.channels();
    fmt.sample_rate = proto_fmt.sample_rate();
    return fmt;
}

pb::AudioFormat to_proto(const AudioFormat& fmt)
{
    pb::AudioFormat proto_fmt;
    switch (fmt.encoding) {
    case AudioEncoding::PcmS16LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S16LE);
        break;
    case AudioEncoding::PcmS32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S32LE);
        break;
    case AudioEncoding::PcmF32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_F32LE);
        break;
    case AudioEncoding::PcmS24LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S24LE);
        break;
    case AudioEncoding::PcmU8:
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
