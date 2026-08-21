#ifndef AQUA_AUDIO_FORMAT_CONVERTER_H
#define AQUA_AUDIO_FORMAT_CONVERTER_H

#include "core/audio/audio_format.h"

#include <aqua_service.pb.h>

namespace aqua {

// proto AudioFormat -> 原生 AudioFormat
// 参数名用 proto_fmt 而非 pb，避免遮蔽命名空间 pb（实现依赖 pb::AudioFormat 枚举）。
audio::AudioFormat from_proto(const pb::AudioFormat& proto_fmt);

// 原生 AudioFormat -> proto AudioFormat
pb::AudioFormat to_proto(const audio::AudioFormat& fmt);

} // namespace aqua::audio

#endif // AQUA_AUDIO_FORMAT_CONVERTER_H
