#include "aqua/audio/depacketizer/audio_depacketizer.h"

#include "aqua/net/udp/network_frame.h"

namespace aqua::audio {

AudioDepacketizer::AudioDepacketizer(JitterBuffer& jb, std::uint32_t frame_count)
    : jb_(jb)
    , frame_count_(frame_count)
{
}

bool AudioDepacketizer::handle_datagram(std::span<const std::byte> datagram) noexcept
{
    const auto nf = net::NetworkFrame::decode(datagram);
    if (!nf || nf->type() != net::PacketType::Audio) {
        return false;
    }
    AudioFrame frame { nf->sequence(), frame_count_, nf->payload() };
    return jb_.push(frame);
}

} // namespace aqua::audio
