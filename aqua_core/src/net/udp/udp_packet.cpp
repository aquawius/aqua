#include "aqua/net/udp/udp_packet.h"

#include <algorithm>
#include <cstring>

namespace aqua::net {

namespace {

// wire 采用 little-endian；所有支持平台（x86/x64/ARM/ARM64）均为 LE，
// 直接 memcpy 即等价于 LE 序列化。若未来支持大端平台，仅需改这里。
std::uint64_t read_u64_le(const std::byte* p) noexcept
{
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::uint32_t read_u32_le(const std::byte* p) noexcept
{
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void write_u64_le(std::byte* p, std::uint64_t v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}

void write_u32_le(std::byte* p, std::uint32_t v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
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

std::vector<std::byte> encode_audio_packet(std::uint64_t sequence, std::span<const std::byte> pcm)
{
    std::vector<std::byte> packet(kAudioHeaderBytes + pcm.size());
    packet[0] = type_byte(PacketType::Audio);
    write_u64_le(packet.data() + kAudioSequenceOffset, sequence);
    std::copy(pcm.begin(), pcm.end(),
        packet.begin() + static_cast<std::ptrdiff_t>(kAudioPayloadOffset));
    return packet;
}

std::vector<std::byte> encode_hello_packet(std::uint32_t session_id)
{
    std::vector<std::byte> packet(kHelloPacketBytes);
    packet[0] = type_byte(PacketType::Hello);
    write_u32_le(packet.data() + kHelloSessionIdOffset, session_id);
    return packet;
}

std::vector<std::byte> encode_hello_ack_packet(std::uint32_t session_id)
{
    std::vector<std::byte> packet(kHelloPacketBytes);
    packet[0] = type_byte(PacketType::HelloAck);
    write_u32_le(packet.data() + kHelloSessionIdOffset, session_id);
    return packet;
}

PacketType decode_packet_type(std::span<const std::byte> packet) noexcept
{
    if (packet.size() < kPacketTypeBytes) {
        return PacketType::Invalid;
    }
    return type_from_byte(packet[0]);
}

bool decode_audio_packet(std::span<const std::byte> packet,
    std::uint64_t& sequence, std::span<const std::byte>& pcm) noexcept
{
    if (packet.size() < kAudioHeaderBytes) {
        return false;
    }
    sequence = read_u64_le(packet.data() + kAudioSequenceOffset);
    pcm = packet.subspan(kAudioPayloadOffset);
    return true;
}

bool decode_hello_packet(std::span<const std::byte> packet, std::uint32_t& session_id) noexcept
{
    if (packet.size() < kHelloPacketBytes) {
        return false;
    }
    session_id = read_u32_le(packet.data() + kHelloSessionIdOffset);
    return true;
}

} // namespace aqua::net
