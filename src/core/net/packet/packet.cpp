#include "core/net/packet/packet.h"

#include <cstring>

namespace aqua::net {

namespace {
    // 小端序读写。x86/x64 原生小端，直接 memcpy。
    // 跨大端平台时需改为字节逐位拼装。

    void write_u32_le(std::byte* p, std::uint32_t v) noexcept
    {
        std::memcpy(p, &v, sizeof(v));
    }

    void write_u16_le(std::byte* p, std::uint16_t v) noexcept
    {
        std::memcpy(p, &v, sizeof(v));
    }

    std::uint32_t read_u32_le(const std::byte* p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    std::uint16_t read_u16_le(const std::byte* p) noexcept
    {
        std::uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }
} // namespace

std::size_t encode_hello(std::uint32_t session_id, std::span<std::byte> out) noexcept
{
    if (out.size() < sizeof(HelloPacket))
        return 0;
    out[0] = std::byte{static_cast<uint8_t>(PacketType::Hello)};
    write_u32_le(out.data() + 1, session_id);
    return sizeof(HelloPacket);
}

std::size_t encode_hello_ack(std::uint32_t session_id, std::span<std::byte> out) noexcept
{
    if (out.size() < sizeof(HelloPacket))
        return 0;
    out[0] = std::byte{static_cast<uint8_t>(PacketType::HelloAck)};
    write_u32_le(out.data() + 1, session_id);
    return sizeof(HelloPacket);
}

std::size_t encode_audio(std::uint32_t session_id,
                         std::uint32_t sequence,
                         std::uint32_t sample_position,
                         std::span<const std::byte> payload,
                         std::span<std::byte> out) noexcept
{
    const std::size_t needed = sizeof(AudioPacketHeader) + payload.size();
    if (out.size() < needed)
        return 0;
    if (payload.size() > 0xFFFF)
        return 0; // payload_size 是 u16

    auto* p = out.data();
    p[0] = std::byte{static_cast<uint8_t>(PacketType::Audio)};
    write_u32_le(p + 1, session_id);
    write_u32_le(p + 5, sequence);
    write_u32_le(p + 9, sample_position);
    write_u16_le(p + 13, static_cast<std::uint16_t>(payload.size()));

    if (!payload.empty()) {
        std::memcpy(p + sizeof(AudioPacketHeader), payload.data(), payload.size());
    }
    return needed;
}

std::optional<PacketType> peek_type(std::span<const std::byte> in) noexcept
{
    if (in.empty())
        return std::nullopt;
    auto t = static_cast<PacketType>(static_cast<uint8_t>(in[0]));
    switch (t) {
    case PacketType::Hello:
    case PacketType::HelloAck:
    case PacketType::Audio:
        return t;
    }
    return std::nullopt;
}

std::optional<HelloPacket> decode_hello(std::span<const std::byte> in) noexcept
{
    if (in.size() < sizeof(HelloPacket))
        return std::nullopt;
    auto t = static_cast<PacketType>(static_cast<uint8_t>(in[0]));
    if (t != PacketType::Hello && t != PacketType::HelloAck)
        return std::nullopt;
    HelloPacket pkt;
    pkt.type = t;
    pkt.session_id = read_u32_le(in.data() + 1);
    return pkt;
}

std::optional<DecodedAudio> decode_audio(std::span<const std::byte> in) noexcept
{
    if (in.size() < sizeof(AudioPacketHeader))
        return std::nullopt;
    auto t = static_cast<PacketType>(static_cast<uint8_t>(in[0]));
    if (t != PacketType::Audio)
        return std::nullopt;

    DecodedAudio result;
    result.header.type = t;
    result.header.session_id = read_u32_le(in.data() + 1);
    result.header.sequence = read_u32_le(in.data() + 5);
    result.header.sample_position = read_u32_le(in.data() + 9);
    result.header.payload_size = read_u16_le(in.data() + 13);

    // 校验 payload 长度
    const std::size_t payload_available = in.size() - sizeof(AudioPacketHeader);
    if (payload_available < result.header.payload_size)
        return std::nullopt;

    result.payload = in.subspan(sizeof(AudioPacketHeader), result.header.payload_size);
    return result;
}

} // namespace aqua::net
