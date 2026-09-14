#include "aqua/audio/audio_format_converter.h"

#include "aqua/audio/audio_error.h"
#include "aqua/logger/logger.h"

namespace aqua::audio {

// proto -> 原生：encoding 枚举一一映射；channels/sample_rate 因 proto3 uint32
// 无范围约束必须显式校验（见下）。非法输入返回 std::unexpected 而非哨兵格式。
std::expected<AudioFormat, AudioError> from_proto(const pb::AudioFormat& proto_fmt)
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
        log_trace_fmt("AudioFormat from proto rejected unknown encoding: {}",
            static_cast<int>(proto_fmt.encoding()));
        return std::unexpected(AudioError::FormatUnsupported);
    }

    // proto3 的 uint32 字段仍可能携带协议层面的非法大值：负数/超大值会经隐式转换变成巨大 uint32，
    // 在下游（frame_bytes / WAVEFORMATEX 截断）造成除零或错误格式。此处显式校验，
    // 复用 AudioFormat::is_valid() 的同一套规则（校验口径只有一处）。
    fmt.channels = proto_fmt.channels();
    fmt.sample_rate = proto_fmt.sample_rate();
    if (!fmt.is_valid()) {
        log_trace_fmt("AudioFormat from proto rejected dimensions: channels={} rate={}",
            fmt.channels, fmt.sample_rate);
        return std::unexpected(AudioError::InvalidArgument);
    }
    log_trace_fmt("AudioFormat from proto: enc={} channels={} rate={}",
        static_cast<int>(fmt.encoding), fmt.channels, fmt.sample_rate);
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
    log_trace_fmt("AudioFormat to proto: enc={} channels={} rate={} valid={}",
        static_cast<int>(fmt.encoding), fmt.channels, fmt.sample_rate, fmt.is_valid());
    return proto_fmt;
}

} // namespace aqua::audio
