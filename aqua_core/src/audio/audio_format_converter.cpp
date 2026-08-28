#include "aqua/audio/audio_format_converter.h"

namespace aqua::audio {

// proto -> 原生：encoding 枚举一一映射；channels/sample_rate 因 proto3 uint32
// 无范围约束必须显式校验（见下），任何非法值最终归一到 INVALID 格式。
AudioFormat from_proto(const pb::AudioFormat& proto_fmt)
{
    AudioFormat fmt;
    switch (proto_fmt.encoding()) {
    case pb::AudioFormat::ENCODING_PCM_S16LE:
        fmt.encoding = AudioEncoding::PCM_S16LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S32LE:
        fmt.encoding = AudioEncoding::PCM_S32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_F32LE:
        fmt.encoding = AudioEncoding::PCM_F32LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_S24LE:
        fmt.encoding = AudioEncoding::PCM_S24LE;
        break;
    case pb::AudioFormat::ENCODING_PCM_U8:
        fmt.encoding = AudioEncoding::PCM_U8;
        break;
    default:
        fmt.encoding = AudioEncoding::INVALID;
        break;
    }

    // proto3 的 uint32 字段仍可能携带协议层面的非法大值：负数/超大值会经隐式转换变成巨大 uint32，
    // 通过 AudioFormat::is_valid() 后在下游（frame_bytes / WAVEFORMATEX 截断）造成
    // 除零或错误格式。此处显式校验。
    const std::uint32_t channels = proto_fmt.channels();
    const std::uint32_t sample_rate = proto_fmt.sample_rate();
    if (channels == 0 || channels > AUDIO_FORMAT_MAX_CHANNELS
        || sample_rate == 0 || sample_rate > AUDIO_FORMAT_MAX_SAMPLE_RATE) {
        // 归一到 INVALID 并清零：调用方以 is_valid() 判失败，避免下游拿到
        // 半合法格式（encoding 有效但 channels/rate 非法）。
        fmt.encoding = AudioEncoding::INVALID;
        fmt.channels = 0;
        fmt.sample_rate = 0;
        return fmt;
    }
    fmt.channels = channels;
    fmt.sample_rate = sample_rate;
    return fmt;
}

// 原生 -> proto：字段直接映射。原生侧枚举与 proto 一一对应
// （见 audio_format.h 注释"枚举值必须保持同步"），INVALID 映射为 ENCODING_INVALID。
pb::AudioFormat to_proto(const AudioFormat& fmt)
{
    pb::AudioFormat proto_fmt;
    switch (fmt.encoding) {
    case AudioEncoding::PCM_S16LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S16LE);
        break;
    case AudioEncoding::PCM_S32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S32LE);
        break;
    case AudioEncoding::PCM_F32LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_F32LE);
        break;
    case AudioEncoding::PCM_S24LE:
        proto_fmt.set_encoding(pb::AudioFormat::ENCODING_PCM_S24LE);
        break;
    case AudioEncoding::PCM_U8:
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

} // namespace aqua::audio
