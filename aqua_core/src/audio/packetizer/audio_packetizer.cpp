#include "aqua/audio/packetizer/audio_packetizer.h"

#include <limits>

namespace aqua::audio {

AudioPacketizer::AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes)
    : frame_bytes_(frame_bytes)
    , frame_count_(frame_count)
{
    if (is_valid_config(frame_count_, frame_bytes_)) {
        pending_.resize(static_cast<std::size_t>(frame_count_) * frame_bytes_);
    }
}

bool AudioPacketizer::is_valid_config(
    std::uint32_t frame_count, std::uint32_t frame_bytes) noexcept
{
    return frame_count != 0
        && frame_bytes != 0
        && static_cast<std::size_t>(frame_count)
            <= std::numeric_limits<std::size_t>::max() / frame_bytes;
}

bool AudioPacketizer::valid() const noexcept
{
    return is_valid_config(frame_count_, frame_bytes_) && !pending_.empty();
}

void AudioPacketizer::reset() noexcept
{
    pending_size_ = 0;
    sequence_ = 0;
}

} // namespace aqua::audio
