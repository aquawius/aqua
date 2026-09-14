#ifndef AQUA_AUDIO_AUDIO_FORMAT_CONVERTER_H
#define AQUA_AUDIO_AUDIO_FORMAT_CONVERTER_H

// protobuf AudioFormat <-> 原生 audio::AudioFormat 双向转换。
// 原生类型用于音频管线内部，避免音频后端直接依赖 protobuf 生成类型；
// 转换发生在 gRPC 边界，并对 proto 侧数据做范围校验（见 from_proto 实现）。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"

#include <aqua_service.pb.h>

#include <expected>

namespace aqua::audio {

// proto AudioFormat -> 原生 AudioFormat
// 参数名用 proto_fmt 而非 pb，避免遮蔽命名空间 pb。
// 失败不再用「encoding=INVALID + 字段清零」的哨兵值表达（调用方可能忘记
// 检查 is_valid() 而把半合法格式传下去），改为 std::expected：
// 无值即失败，[[nodiscard]] 使忽略返回值成为显式行为。
//   - 未知 / 非法 encoding → AudioError::FormatUnsupported
//   - channels / sample_rate 越界（proto3 uint32 无范围约束）→ AudioError::InvalidArgument
[[nodiscard]] std::expected<AudioFormat, AudioError> from_proto(const pb::AudioFormat& proto_fmt);

// 原生 AudioFormat -> proto AudioFormat
// 字段直接映射、无校验；INVALID 编码映射为 ENCODING_INVALID。
pb::AudioFormat to_proto(const AudioFormat& fmt);

} // namespace aqua::audio

#endif // AQUA_AUDIO_AUDIO_FORMAT_CONVERTER_H
