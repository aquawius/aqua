#include "aqua/audio/depacketizer/audio_depacketizer.h"

#include "aqua/net/udp/udp_packet.h"

namespace aqua::audio {

AudioDepacketizer::AudioDepacketizer(JitterBuffer& jb, std::uint32_t frame_count)
    : jb_(jb)
    , frame_count_(frame_count)
{
}

bool AudioDepacketizer::handle_datagram(std::span<const std::byte> datagram) noexcept
{
    if (net::decode_packet_type(datagram) != net::PacketType::Audio) {
        return false;
    }
    std::uint64_t sequence = 0;
    std::span<const std::byte> pcm;
    if (!net::decode_audio_packet(datagram, sequence, pcm)) {
        return false;
    }
    // timestamp_ns 为保留字段，构造时置 0（JB 不使用）。
    AudioFrame frame { sequence, 0, frame_count_, pcm };
    return jb_.push(frame);
}

} // namespace aqua::audio
