#include "aqua/net/udp/network_frame.h"

#include "aqua/net/udp/udp_config.h"

#include <algorithm>

namespace aqua::net {

namespace {

    // RTP 头字段显式使用大端编码（RFC 3550），实现不依赖主机字节序。
    // Hello/Ack 沿用既有小端 5-byte 布局（控制面遗留，不动）。
    std::uint16_t read_u16_be(const std::byte* p) noexcept
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8)
            | std::to_integer<std::uint8_t>(p[1]));
    }

    std::uint32_t read_u32_be(const std::byte* p) noexcept
    {
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8)
            | static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
    }

    void write_u16_be(std::byte* p, std::uint16_t v) noexcept
    {
        p[0] = static_cast<std::byte>((v >> 8) & 0xFFu);
        p[1] = static_cast<std::byte>(v & 0xFFu);
    }

    void write_u32_be(std::byte* p, std::uint32_t v) noexcept
    {
        for (unsigned i = 0; i < 4; ++i) {
            p[i] = static_cast<std::byte>((v >> ((3 - i) * 8)) & 0xFFu);
        }
    }

    std::uint32_t read_u32_le(const std::byte* p) noexcept
    {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0]))
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
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
        case 4:
            return PacketType::Heartbeat;
        default:
            return PacketType::Invalid;
        }
    }

} // namespace

NetworkFrame NetworkFrame::audio(std::uint16_t sequence, std::uint32_t timestamp,
    std::uint32_t ssrc, std::span<const std::byte> payload)
{
    NetworkFrame f;
    f.type_ = PacketType::Audio;
    f.rtp_sequence_ = sequence;
    f.timestamp_ = timestamp;
    f.ssrc_ = ssrc;
    f.payload_type_ = kRtpPayloadTypePcm;
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

NetworkFrame NetworkFrame::heartbeat(std::uint32_t session_id)
{
    NetworkFrame f;
    f.type_ = PacketType::Heartbeat;
    f.session_id_ = session_id;
    return f;
}

std::vector<std::byte> NetworkFrame::encode() const
{
    switch (type_) {
    case PacketType::Audio: {
        if (payload_.empty() || payload_.size() > config::UDP_AUDIO_PAYLOAD_BYTES) {
            return { };
        }
        std::vector<std::byte> packet(kRtpHeaderBytes + payload_.size());
        packet[0] = std::byte { 0x80 }; // V=2, P/X/CC=0
        packet[1] = static_cast<std::byte>(payload_type_ & 0x7F); // M=0
        write_u16_be(packet.data() + kRtpSequenceOffset, rtp_sequence_);
        write_u32_be(packet.data() + kRtpTimestampOffset, timestamp_);
        write_u32_be(packet.data() + kRtpSsrcOffset, ssrc_);
        std::copy(payload_.begin(), payload_.end(),
            packet.begin() + static_cast<std::ptrdiff_t>(kRtpPayloadOffset));
        return packet;
    }
    case PacketType::Hello:
    case PacketType::HelloAck:
    case PacketType::Heartbeat: {
        std::vector<std::byte> packet(kHelloPacketBytes);
        packet[0] = type_byte(type_);
        write_u32_le(packet.data() + kHelloSessionIdOffset, session_id_);
        return packet;
    }
    case PacketType::Invalid:
    default:
        return { };
    }
}

std::optional<NetworkFrame> NetworkFrame::decode(std::span<const std::byte> wire) noexcept
{
    if (wire.empty()) {
        return std::nullopt;
    }

    // RTP 音频包：首字节 0x80（V=2, P/X/CC=0）。
    if (std::to_integer<std::uint8_t>(wire[0]) == 0x80) {
        if (wire.size() <= kRtpHeaderBytes
            || wire.size() - kRtpHeaderBytes > config::UDP_AUDIO_PAYLOAD_BYTES) {
            return std::nullopt;
        }
        const auto b1 = std::to_integer<std::uint8_t>(wire[1]);
        if ((b1 & 0x80) != 0) {
            return std::nullopt; // M 恒 0：不断流语义，不接受带外标记
        }
        if ((b1 & 0x7F) != kRtpPayloadTypePcm) {
            return std::nullopt; // 仅 PCM-LE（Opus 预留 97，后续版本）
        }
        NetworkFrame f;
        f.type_ = PacketType::Audio;
        f.payload_type_ = static_cast<std::uint8_t>(b1 & 0x7F);
        f.rtp_sequence_ = read_u16_be(wire.data() + kRtpSequenceOffset);
        f.timestamp_ = read_u32_be(wire.data() + kRtpTimestampOffset);
        f.ssrc_ = read_u32_be(wire.data() + kRtpSsrcOffset);
        f.payload_ = wire.subspan(kRtpPayloadOffset);
        return f;
    }

    const PacketType type = type_from_byte(wire[0]);
    NetworkFrame f;

    switch (type) {
    case PacketType::Hello:
    case PacketType::HelloAck:
    case PacketType::Heartbeat:
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
