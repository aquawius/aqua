#include "aqua/net/udp/network_frame.h"

#include <algorithm>

namespace aqua::net {

namespace {

// wire uses explicit little-endian encoding; the implementation does not depend on
// host endianness.
std::uint64_t read_u64_le(const std::byte* p) noexcept
{
    return static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[0]))
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[1])) << 8)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[2])) << 16)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[3])) << 24)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[4])) << 32)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[5])) << 40)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[6])) << 48)
        | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[7])) << 56);
}

std::uint32_t read_u32_le(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0]))
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}

void write_u64_le(std::byte* p, std::uint64_t v) noexcept
{
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = static_cast<std::byte>((v >> (i * 8)) & 0xFFu);
    }
}

void write_u32_le(std::byte* p, std::uint32_t v) noexcept
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<std::byte>((v >> (i * 8)) & 0xFFu);
    }
}

std::byte type_byte(PacketType t) noexcept
{
    return static_cast<std::byte>(static_cast<std::uint8_t>(t));
}

PacketType type_from_byte(std::byte b) noexcept
{
    switch (std::to_integer<std::uint8_t>(b)) {
    case 1:
        return PacketType::Hello;
    case 2:
        return PacketType::HelloAck;
    case 3:
        return PacketType::Audio;
    default:
        return PacketType::Invalid;
    }
}

} // namespace

NetworkFrame NetworkFrame::audio(std::uint64_t sequence, std::span<const std::byte> payload)
{
    NetworkFrame f;
    f.type_ = PacketType::Audio;
    f.sequence_ = sequence;
    f.payload_ = payload;
    return f;
}

NetworkFrame NetworkFrame::hello(std::uint32_t session_id)
{
    NetworkFrame f;
    f.type_ = PacketType::Hello;
    f.session_id_ = session_id;
    return f;
}

NetworkFrame NetworkFrame::hello_ack(std::uint32_t session_id)
{
    NetworkFrame f;
    f.type_ = PacketType::HelloAck;
    f.session_id_ = session_id;
    return f;
}

std::vector<std::byte> NetworkFrame::encode() const
{
    switch (type_) {
    case PacketType::Audio: {
        std::vector<std::byte> packet(kAudioHeaderBytes + payload_.size());
        packet[0] = type_byte(PacketType::Audio);
        write_u64_le(packet.data() + kAudioSequenceOffset, sequence_);
        std::copy(payload_.begin(), payload_.end(),
            packet.begin() + static_cast<std::ptrdiff_t>(kAudioPayloadOffset));
        return packet;
    }
    case PacketType::Hello:
    case PacketType::HelloAck: {
        std::vector<std::byte> packet(kHelloPacketBytes);
        packet[0] = type_byte(type_);
        write_u32_le(packet.data() + kHelloSessionIdOffset, session_id_);
        return packet;
    }
    case PacketType::Invalid:
    default:
        return {};
    }
}

std::optional<NetworkFrame> NetworkFrame::decode(std::span<const std::byte> wire) noexcept
{
    if (wire.size() < kPacketTypeBytes) {
        return std::nullopt;
    }

    const PacketType type = type_from_byte(wire[0]);
    NetworkFrame f;

    switch (type) {
    case PacketType::Audio:
        if (wire.size() <= kAudioHeaderBytes) {
            return std::nullopt;
        }
        f.type_ = PacketType::Audio;
        f.sequence_ = read_u64_le(wire.data() + kAudioSequenceOffset);
        f.payload_ = wire.subspan(kAudioPayloadOffset);
        return f;
    case PacketType::Hello:
    case PacketType::HelloAck:
        if (wire.size() != kHelloPacketBytes) {
            return std::nullopt;
        }
        f.type_ = type;
        f.session_id_ = read_u32_le(wire.data() + kHelloSessionIdOffset);
        return f;
    default:
        return std::nullopt;
    }
}

} // namespace aqua::net
