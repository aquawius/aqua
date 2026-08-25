#include "aqua/net/udp/udp_packet.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using aqua::net::PacketType;

TEST(UdpPacketTest, AudioRoundTrip)
{
    std::array<std::byte, 16> pcm {};
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::byte>(i);
    }
    const auto packet = aqua::net::encode_audio_packet(0x123456789ABCDEF0ull, pcm);

    EXPECT_EQ(packet.size(), 9u + 16u);
    EXPECT_EQ(aqua::net::decode_packet_type(packet), PacketType::Audio);

    std::uint64_t seq = 0;
    std::span<const std::byte> payload;
    ASSERT_TRUE(aqua::net::decode_audio_packet(packet, seq, payload));
    EXPECT_EQ(seq, 0x123456789ABCDEF0ull);
    ASSERT_EQ(payload.size(), 16u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(payload[5]), 5u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(payload[15]), 15u);
}

TEST(UdpPacketTest, HelloRoundTrip)
{
    const auto packet = aqua::net::encode_hello_packet(0xDEADBEEFu);
    EXPECT_EQ(packet.size(), 5u);
    EXPECT_EQ(aqua::net::decode_packet_type(packet), PacketType::Hello);

    std::uint32_t sid = 0;
    ASSERT_TRUE(aqua::net::decode_hello_packet(packet, sid));
    EXPECT_EQ(sid, 0xDEADBEEFu);
}

TEST(UdpPacketTest, HelloAckRoundTrip)
{
    const auto packet = aqua::net::encode_hello_ack_packet(0x12345678u);
    EXPECT_EQ(packet.size(), 5u);
    EXPECT_EQ(aqua::net::decode_packet_type(packet), PacketType::HelloAck);

    std::uint32_t sid = 0;
    ASSERT_TRUE(aqua::net::decode_hello_packet(packet, sid));
    EXPECT_EQ(sid, 0x12345678u);
}

TEST(UdpPacketTest, DecodeTypeRejectsShortOrUnknown)
{
    EXPECT_EQ(aqua::net::decode_packet_type(std::span<const std::byte> {}), PacketType::Invalid);

    const std::array<std::byte, 1> unknown { std::byte { 0x7F } };
    EXPECT_EQ(aqua::net::decode_packet_type(unknown), PacketType::Invalid);
}

TEST(UdpPacketTest, DecodeAudioRejectsShort)
{
    const std::array<std::byte, 8> short_packet {};
    std::uint64_t seq = 0;
    std::span<const std::byte> pcm;
    EXPECT_FALSE(aqua::net::decode_audio_packet(short_packet, seq, pcm));
}

TEST(UdpPacketTest, DecodeHelloRejectsShort)
{
    const std::array<std::byte, 4> short_packet {};
    std::uint32_t sid = 0;
    EXPECT_FALSE(aqua::net::decode_hello_packet(short_packet, sid));
}

TEST(UdpPacketTest, PayloadIsViewNotCopy)
{
    const std::array<std::byte, 4> pcm { std::byte { 1 }, std::byte { 2 }, std::byte { 3 },
        std::byte { 4 } };
    auto packet = aqua::net::encode_audio_packet(7, pcm);

    std::uint64_t seq = 0;
    std::span<const std::byte> view;
    ASSERT_TRUE(aqua::net::decode_audio_packet(packet, seq, view));
    // payload 应指向 packet 内部，而非独立拷贝。
    EXPECT_EQ(view.data(), packet.data() + 9);
    EXPECT_EQ(std::to_integer<std::uint8_t>(view[2]), 3u);
}

} // namespace
