#ifndef AQUA_AUDIO_FORMAT_CONVERTER_H
#define AQUA_AUDIO_FORMAT_CONVERTER_H

// protobuf AudioFormat <-> 原生 audio::AudioFormat 双向转换。
// 原生类型用于音频管线内部，避免音频后端直接依赖 protobuf 生成类型；
// 转换发生在 gRPC 边界，并对 proto 侧数据做范围校验（见 from_proto 实现）。

#include "aqua/audio/audio_format.h"

#include <aqua_service.pb.h>

namespace aqua {

// proto AudioFormat -> 原生 AudioFormat
// 参数名用 proto_fmt 而非 pb，避免遮蔽命名空间 pb（实现依赖 pb::AudioFormat 枚举）。
// 非法 encoding / channels / sample_rate 一律归一到 encoding=INVALID 的格式，
// 调用方通过 audio::AudioFormat::is_valid() 判失败。
audio::AudioFormat from_proto(const pb::AudioFormat& proto_fmt);

// 原生 AudioFormat -> proto AudioFormat
// 字段直接映射、无校验；INVALID 编码映射为 ENCODING_INVALID。
pb::AudioFormat to_proto(const audio::AudioFormat& fmt);

} // namespace aqua::audio

#endif // AQUA_AUDIO_FORMAT_CONVERTER_H
