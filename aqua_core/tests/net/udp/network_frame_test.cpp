#include "aqua/net/udp/network_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using aqua::net::NetworkFrame;
using aqua::net::PacketType;

TEST(NetworkFrameTest, AudioRoundTrip)
{
    std::array<std::byte, 16> pcm {};
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::byte>(i);
    }
    const auto packet = NetworkFrame::audio(0x123456789ABCDEF0ull, pcm).encode();

    EXPECT_EQ(packet.size(), aqua::net::kAudioHeaderBytes + 16u);
    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type(), PacketType::Audio);
    EXPECT_EQ(decoded->sequence(), 0x123456789ABCDEF0ull);
    ASSERT_EQ(decoded->payload().size(), 16u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(decoded->payload()[5]), 5u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(decoded->payload()[15]), 15u);
}

TEST(NetworkFrameTest, HelloRoundTrip)
{
    const auto packet = NetworkFrame::hello(0xDEADBEEFu).encode();
    EXPECT_EQ(packet.size(), 5u);
    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type(), PacketType::Hello);
    EXPECT_EQ(decoded->session_id(), 0xDEADBEEFu);
}

TEST(NetworkFrameTest, HelloAckRoundTrip)
{
    const auto packet = NetworkFrame::hello_ack(0x12345678u).encode();
    EXPECT_EQ(packet.size(), 5u);
    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type(), PacketType::HelloAck);
    EXPECT_EQ(decoded->session_id(), 0x12345678u);
}

TEST(NetworkFrameTest, DecodeRejectsShortOrUnknown)
{
    EXPECT_FALSE(NetworkFrame::decode(std::span<const std::byte> {}).has_value());

    const std::array<std::byte, 1> unknown { std::byte { 0x7F } };
    EXPECT_FALSE(NetworkFrame::decode(unknown).has_value());
}

TEST(NetworkFrameTest, DecodeAudioRejectsShort)
{
    const std::array<std::byte, 8> short_packet {};
    EXPECT_FALSE(NetworkFrame::decode(short_packet).has_value());
}

TEST(NetworkFrameTest, DecodeHelloRejectsShort)
{
    const std::array<std::byte, 4> short_packet {};
    EXPECT_FALSE(NetworkFrame::decode(short_packet).has_value());
}

TEST(NetworkFrameTest, PayloadIsViewNotCopy)
{
    const std::array<std::byte, 4> pcm { std::byte { 1 }, std::byte { 2 }, std::byte { 3 },
        std::byte { 4 } };
    auto packet = NetworkFrame::audio(7, pcm).encode();

    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    // payload 应指向 packet 内部，而非独立拷贝。
    EXPECT_EQ(decoded->payload().data(), packet.data() + 9);
    EXPECT_EQ(std::to_integer<std::uint8_t>(decoded->payload()[2]), 3u);
}

} // namespace
