#include "core/grpc/audio_format_converter.h"

namespace aqua {

AudioFormat from_proto(const pb::AudioFormat& pb)
{
    AudioFormat fmt;
    switch (pb.encoding()) {
    case pb::AudioFormat::ENCODING_PCM_S16LE: fmt.encoding = AudioEncoding::PcmS16LE; break;
    case pb::AudioFormat::ENCODING_PCM_S32LE: fmt.encoding = AudioEncoding::PcmS32LE; break;
    case pb::AudioFormat::ENCODING_PCM_F32LE: fmt.encoding = AudioEncoding::PcmF32LE; break;
    case pb::AudioFormat::ENCODING_PCM_S24LE: fmt.encoding = AudioEncoding::PcmS24LE; break;
    case pb::AudioFormat::ENCODING_PCM_U8:    fmt.encoding = AudioEncoding::PcmU8;    break;
    default: fmt.encoding = AudioEncoding::Invalid; break;
    }
    fmt.channels = pb.channels();
    fmt.sample_rate = pb.sample_rate();
    return fmt;
}

pb::AudioFormat to_proto(const AudioFormat& fmt)
{
    pb::AudioFormat pb;
    switch (fmt.encoding) {
    case AudioEncoding::PcmS16LE: pb.set_encoding(pb::AudioFormat::ENCODING_PCM_S16LE); break;
    case AudioEncoding::PcmS32LE: pb.set_encoding(pb::AudioFormat::ENCODING_PCM_S32LE); break;
    case AudioEncoding::PcmF32LE: pb.set_encoding(pb::AudioFormat::ENCODING_PCM_F32LE); break;
    case AudioEncoding::PcmS24LE: pb.set_encoding(pb::AudioFormat::ENCODING_PCM_S24LE); break;
    case AudioEncoding::PcmU8:    pb.set_encoding(pb::AudioFormat::ENCODING_PCM_U8);    break;
    default: pb.set_encoding(pb::AudioFormat::ENCODING_INVALID); break;
    }
    pb.set_channels(fmt.channels);
    pb.set_sample_rate(fmt.sample_rate);
    return pb;
}

} // namespace aqua
