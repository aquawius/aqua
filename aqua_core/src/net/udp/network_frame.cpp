#include "aqua/net/udp/network_frame.h"

#include "aqua/net/udp/udp_config.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace aqua::net {

namespace {

    // RTP 头字段：标准大端（std::byteswap，C++23；实现不依赖主机字节序）。
    // 定长 span 参数把长度约束写进类型；调用点的 subspan<offset, N>() 边界
    // 由 decode 入口的既有长度校验保证（memcpy 而非 bit_cast：span 非平凡可复制）。
    std::uint16_t read_u16_be(std::span<const std::byte, 2> p) noexcept
    {
        std::uint16_t v;
        std::memcpy(&v, p.data(), sizeof(v));
        return std::byteswap(v);
    }

    std::uint32_t read_u32_be(std::span<const std::byte, 4> p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p.data(), sizeof(v));
        return std::byteswap(v);
    }

    void write_u16_be(std::span<std::byte, 2> p, std::uint16_t v) noexcept
    {
        const auto be = std::byteswap(v);
        std::memcpy(p.data(), &be, sizeof(be));
    }

    void write_u32_be(std::span<std::byte, 4> p, std::uint32_t v) noexcept
    {
        const auto be = std::byteswap(v);
        std::memcpy(p.data(), &be, sizeof(be));
    }

    // 5-byte 控制包沿用小端（遗留 convention，与 RTP 头无关）。
    // 以下 memcpy 实现要求主机小端；全支持平台（x86/x64/ARM/ARM64）均为 LE。
    static_assert(std::endian::native == std::endian::little,
        "control-plane LE helpers require a little-endian host");
    std::uint32_t read_u32_le(std::span<const std::byte, 4> p) noexcept
    {
        std::uint32_t v;
        std::memcpy(&v, p.data(), sizeof(v));
        return v;
    }

    void write_u32_le(std::span<std::byte, 4> p, std::uint32_t v) noexcept
    {
        std::memcpy(p.data(), &v, sizeof(v));
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
        const std::span<std::byte> wire { packet };
        write_u16_be(wire.subspan<kRtpSequenceOffset, 2>(), rtp_sequence_);
        write_u32_be(wire.subspan<kRtpTimestampOffset, 4>(), timestamp_);
        write_u32_be(wire.subspan<kRtpSsrcOffset, 4>(), ssrc_);
        std::ranges::copy(payload_, wire.subspan<kRtpPayloadOffset>().begin());
        return packet;
    }
    case PacketType::Heartbeat:
    case PacketType::HeartbeatAck: {
        std::vector<std::byte> packet(kHeartbeatPacketBytes);
        packet[0] = static_cast<std::byte>(static_cast<std::uint8_t>(type_));
        write_u32_le(std::span { packet }.subspan<kHeartbeatSessionIdOffset, 4>(), session_id_);
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
        f.rtp_sequence_ = read_u16_be(wire.subspan<kRtpSequenceOffset, 2>());
        f.timestamp_ = read_u32_be(wire.subspan<kRtpTimestampOffset, 4>());
        f.ssrc_ = read_u32_be(wire.subspan<kRtpSsrcOffset, 4>());
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
        f.session_id_ = read_u32_le(wire.subspan<kHeartbeatSessionIdOffset, 4>());
        return f;
    default:
        return std::nullopt;
    }
}

} // namespace aqua::net
