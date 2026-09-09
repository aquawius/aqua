#include "aqua/net/udp/network_frame.h"

#include "aqua/net/udp/udp_config.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace aqua::net {

namespace {

    // RTP 头字段：标准大端（std::byteswap，C++23；实现不依赖主机字节序）。
    std::uint16_t read_u16_be(const std::byte* p) noexcept
    {
        std::uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        return std::byteswap(v);
    }

    std::uint32_t read_u32_be(const std::byte* p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return std::byteswap(v);
    }

    void write_u16_be(std::byte* p, std::uint16_t v) noexcept
    {
        const auto be = std::byteswap(v);
        std::memcpy(p, &be, sizeof(be));
    }

    void write_u32_be(std::byte* p, std::uint32_t v) noexcept
    {
        const auto be = std::byteswap(v);
        std::memcpy(p, &be, sizeof(be));
    }

    // 5-byte 控制包沿用小端（遗留 convention，与 RTP 头无关）。
    // 以下 memcpy 实现要求主机小端；全支持平台（x86/x64/ARM/ARM64）均为 LE。
    static_assert(std::endian::native == std::endian::little,
        "control-plane LE helpers require a little-endian host");
    std::uint32_t read_u32_le(const std::byte* p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    void write_u32_le(std::byte* p, std::uint32_t v) noexcept
    {
        std::memcpy(p, &v, sizeof(v));
    }

    PacketType type_from_byte(std::byte b) noexcept
    {
        switch (std::to_integer<std::uint8_t>(b)) {
        case 1:
            return PacketType::Heartbeat;
        case 2:
            return PacketType::HeartbeatAck;
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

NetworkFrame NetworkFrame::heartbeat(std::uint32_t session_id)
{
    NetworkFrame f;
    f.type_ = PacketType::Heartbeat;
    f.session_id_ = session_id;
    return f;
}

NetworkFrame NetworkFrame::heartbeat_ack(std::uint32_t session_id)
{
    NetworkFrame f;
    f.type_ = PacketType::HeartbeatAck;
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
    case PacketType::Heartbeat:
    case PacketType::HeartbeatAck: {
        std::vector<std::byte> packet(kHeartbeatPacketBytes);
        packet[0] = static_cast<std::byte>(static_cast<std::uint8_t>(type_));
        write_u32_le(packet.data() + kHeartbeatSessionIdOffset, session_id_);
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
    case PacketType::Heartbeat:
    case PacketType::HeartbeatAck:
        if (wire.size() != kHeartbeatPacketBytes) {
            return std::nullopt;
        }
        f.type_ = type;
        f.session_id_ = read_u32_le(wire.data() + kHeartbeatSessionIdOffset);
        return f;
    default:
        return std::nullopt;
    }
}

} // namespace aqua::net
