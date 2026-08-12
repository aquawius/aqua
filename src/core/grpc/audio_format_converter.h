#ifndef AQUA_AUDIO_FORMAT_CONVERTER_H
#define AQUA_AUDIO_FORMAT_CONVERTER_H

#include "core/public/audio_format.h"

#include <aqua_service.pb.h>

namespace aqua {

// proto AudioFormat -> 原生 AudioFormat
AudioFormat from_proto(const pb::AudioFormat& pb);

// 原生 AudioFormat -> proto AudioFormat
pb::AudioFormat to_proto(const AudioFormat& fmt);

} // namespace aqua

#endif // AQUA_AUDIO_FORMAT_CONVERTER_H
