#include <gtest/gtest.h>

#include "core/net/packet/packet.h"

#include <array>
#include <vector>

using aqua::net::AudioPacketHeader;
using aqua::net::DecodedAudio;
using aqua::net::HelloPacket;
using aqua::net::PacketType;

TEST(PacketTest, EncodeDecodeHello)
{
    std::array<std::byte, 64> buf{};
    auto written = aqua::net::encode_hello(0x12345678, buf);
    EXPECT_EQ(written, sizeof(HelloPacket));

    auto decoded = aqua::net::decode_hello(std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, PacketType::Hello);
    EXPECT_EQ(decoded->session_id, 0x12345678u);
}

TEST(PacketTest, EncodeHelloBufferTooSmall)
{
    std::array<std::byte, 2> buf{};
    auto written = aqua::net::encode_hello(1, buf);
    EXPECT_EQ(written, 0u);
}

TEST(PacketTest, DecodeHelloTooShort)
{
    std::array<std::byte, 3> buf{};
    auto decoded = aqua::net::decode_hello(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(PacketTest, DecodeHelloWrongType)
{
    std::array<std::byte, 16> buf{};
    buf[0] = std::byte{99}; // 非法 type
    auto decoded = aqua::net::decode_hello(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(PacketTest, PeekType)
{
    std::array<std::byte, 16> buf{};
    buf[0] = std::byte{1};
    EXPECT_EQ(aqua::net::peek_type(buf), PacketType::Hello);

    buf[0] = std::byte{3};
    EXPECT_EQ(aqua::net::peek_type(buf), PacketType::Audio);

    buf[0] = std::byte{99};
    EXPECT_FALSE(aqua::net::peek_type(buf).has_value());

    EXPECT_FALSE(aqua::net::peek_type({}).has_value());
}

TEST(PacketTest, EncodeDecodeAudio)
{
    std::vector<std::byte> payload(480, std::byte{0x42});
    std::vector<std::byte> buf(64 + payload.size());

    auto written = aqua::net::encode_audio(
        0x7A310001, 42, 960, payload, buf);
    EXPECT_EQ(written, sizeof(AudioPacketHeader) + payload.size());

    auto decoded = aqua::net::decode_audio(
        std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.type, PacketType::Audio);
    EXPECT_EQ(decoded->header.session_id, 0x7A310001u);
    EXPECT_EQ(decoded->header.sequence, 42u);
    EXPECT_EQ(decoded->header.sample_position, 960u);
    EXPECT_EQ(decoded->header.payload_size, 480u);
    EXPECT_EQ(decoded->payload.size(), 480u);
    EXPECT_EQ(decoded->payload[0], std::byte{0x42});
    EXPECT_EQ(decoded->payload[479], std::byte{0x42});
}

TEST(PacketTest, EncodeAudioBufferTooSmall)
{
    std::vector<std::byte> payload(100);
    std::vector<std::byte> buf(10); // 太小
    auto written = aqua::net::encode_audio(1, 1, 1, payload, buf);
    EXPECT_EQ(written, 0u);
}

TEST(PacketTest, EncodeAudioPayloadTooLarge)
{
    std::vector<std::byte> payload(0x10000); // 超过 u16
    std::vector<std::byte> buf(0x10000 + 64);
    auto written = aqua::net::encode_audio(1, 1, 1, payload, buf);
    EXPECT_EQ(written, 0u);
}

TEST(PacketTest, DecodeAudioTooShort)
{
    std::array<std::byte, 5> buf{};
    auto decoded = aqua::net::decode_audio(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(PacketTest, DecodeAudioPayloadTruncated)
{
    // 声称 payload 1000 但实际只有 10
    std::array<std::byte, sizeof(AudioPacketHeader) + 10> buf{};
    buf[0] = std::byte{3};
    // 写 payload_size = 1000 (LE)
    buf[13] = std::byte{0xE8};
    buf[14] = std::byte{0x03};
    auto decoded = aqua::net::decode_audio(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(PacketTest, DecodeAudioWrongType)
{
    std::array<std::byte, sizeof(AudioPacketHeader) + 10> buf{};
    buf[0] = std::byte{1}; // Hello, 不是 Audio
    auto decoded = aqua::net::decode_audio(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(PacketTest, EncodeDecodeAudioZeroPayload)
{
    std::vector<std::byte> buf(64);
    auto written = aqua::net::encode_audio(5, 0, 0, {}, buf);
    EXPECT_EQ(written, sizeof(AudioPacketHeader));

    auto decoded = aqua::net::decode_audio(
        std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.payload_size, 0u);
    EXPECT_EQ(decoded->payload.size(), 0u);
}
