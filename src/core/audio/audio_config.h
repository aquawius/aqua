#ifndef AQUA_AUDIO_CONFIG_H
#define AQUA_AUDIO_CONFIG_H

#include <cstdint>

namespace aqua::audio {

inline constexpr std::uint32_t AUDIO_MAX_CHANNELS = 64;
inline constexpr std::uint32_t AUDIO_MAX_SAMPLE_RATE = 768000;

} // namespace aqua::audio


#endif // AQUA_AUDIO_CONFIG_H
