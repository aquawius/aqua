#include "aqua/net/udp/udp_packet.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

using aqua::net::PacketType;

TEST(UdpPacketBoundaryTest, SequenceExtremes)
{
    for (const auto seq : { 0ULL, std::numeric_limits<std::uint64_t>::max() }) {
        const auto pkt = aqua::net::encode_audio_packet(seq, std::span<const std::byte> {});
        std::uint64_t got = 0;
        std::span<const std::byte> pcm;
        ASSERT_TRUE(aqua::net::decode_audio_packet(pkt, got, pcm));
        EXPECT_EQ(got, seq);
    }
}

TEST(UdpPacketBoundaryTest, SessionIdExtremes)
{
    for (const auto sid : { 0u, std::numeric_limits<std::uint32_t>::max() }) {
        const auto pkt = aqua::net::encode_hello_packet(sid);
        std::uint32_t got = 0;
        ASSERT_TRUE(aqua::net::decode_hello_packet(pkt, got));
        EXPECT_EQ(got, sid);
    }
}

TEST(UdpPacketBoundaryTest, EmptyAudioPayload)
{
    const auto pkt = aqua::net::encode_audio_packet(5, std::span<const std::byte> {});
    EXPECT_EQ(pkt.size(), 9u); // 仅 header
    std::uint64_t seq = 0;
    std::span<const std::byte> pcm;
    ASSERT_TRUE(aqua::net::decode_audio_packet(pkt, seq, pcm));
    EXPECT_EQ(seq, 5u);
    EXPECT_EQ(pcm.size(), 0u);
}

TEST(UdpPacketBoundaryTest, ExactHeaderLengthBoundary)
{
    // 8 字节（缺 1 字节 sequence）→ 解码失败。
    std::array<std::byte, 8> short_pkt {};
    short_pkt[0] = static_cast<std::byte>(PacketType::Audio);
    std::uint64_t seq = 0;
    std::span<const std::byte> pcm;
    EXPECT_FALSE(aqua::net::decode_audio_packet(short_pkt, seq, pcm));

    // 9 字节（恰好 header）→ 解码成功（空 payload）。
    std::array<std::byte, 9> exact {};
    exact[0] = static_cast<std::byte>(PacketType::Audio);
    ASSERT_TRUE(aqua::net::decode_audio_packet(exact, seq, pcm));
    EXPECT_EQ(pcm.size(), 0u);
}

TEST(UdpPacketBoundaryTest, UnknownTypeByte)
{
    const std::array<std::byte, 1> unknown { std::byte { 0xFF } };
    EXPECT_EQ(aqua::net::decode_packet_type(unknown), PacketType::Invalid);
}

} // namespace
