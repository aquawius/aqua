#include "aqua/audio/packetizer/audio_packetizer.h"

#include "aqua/logger/logger.h"

#include <limits>

namespace aqua::audio {

AudioPacketizer::AudioPacketizer(std::uint32_t frame_count, std::uint32_t frame_bytes)
    : frame_bytes_(frame_bytes)
    , frame_count_(frame_count)
{
    if (is_valid_config(frame_count_, frame_bytes_)) {
        pending_.resize(static_cast<std::size_t>(frame_count_) * frame_bytes_);
    }
    log_debug_fmt("AudioPacketizer created: frame_count={} frame_bytes={} pending_bytes={} valid={}",
        frame_count_, frame_bytes_, pending_.size(), valid());
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
    sequence_.store(0, std::memory_order_relaxed);
    input_blocks_.store(0, std::memory_order_relaxed);
    input_bytes_.store(0, std::memory_order_relaxed);
    rejected_unaligned_blocks_.store(0, std::memory_order_relaxed);
    log_debug_fmt("AudioPacketizer reset: frame_count={} frame_bytes={} sequence=0",
        frame_count_, frame_bytes_);
}

} // namespace aqua::audio
