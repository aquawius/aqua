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

// ---- HelloAck 编解码（此前未覆盖）----

TEST(PacketTest, EncodeDecodeHelloAck)
{
    std::array<std::byte, 64> buf{};
    auto written = aqua::net::encode_hello_ack(0xCAFEBABE, buf);
    EXPECT_EQ(written, sizeof(HelloPacket));

    auto decoded = aqua::net::decode_hello(std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, PacketType::HelloAck);
    EXPECT_EQ(decoded->session_id, 0xCAFEBABEu);
}

TEST(PacketTest, EncodeHelloAckBufferTooSmall)
{
    std::array<std::byte, 2> buf{};
    auto written = aqua::net::encode_hello_ack(1, buf);
    EXPECT_EQ(written, 0u);
}

TEST(PacketTest, HelloAckWireFormat)
{
    // 字节级校验：type=2, session_id=0x01020304 (LE -> 04 03 02 01)
    std::array<std::byte, 8> buf{};
    auto written = aqua::net::encode_hello_ack(0x01020304u, buf);
    ASSERT_EQ(written, sizeof(HelloPacket));
    EXPECT_EQ(buf[0], std::byte{0x02}); // HelloAck
    EXPECT_EQ(buf[1], std::byte{0x04});
    EXPECT_EQ(buf[2], std::byte{0x03});
    EXPECT_EQ(buf[3], std::byte{0x02});
    EXPECT_EQ(buf[4], std::byte{0x01});
}

TEST(PacketTest, HelloWireFormat)
{
    // 字节级校验：type=1, session_id=0xFFFFFFFF (全 0xFF)
    std::array<std::byte, 8> buf{};
    auto written = aqua::net::encode_hello(0xFFFFFFFFu, buf);
    ASSERT_EQ(written, sizeof(HelloPacket));
    EXPECT_EQ(buf[0], std::byte{0x01}); // Hello
    EXPECT_EQ(buf[1], std::byte{0xFF});
    EXPECT_EQ(buf[2], std::byte{0xFF});
    EXPECT_EQ(buf[3], std::byte{0xFF});
    EXPECT_EQ(buf[4], std::byte{0xFF});
}

TEST(PacketTest, DecodeHelloRejectsAudioType)
{
    // 用 Audio type 字节解析 hello，应失败
    std::array<std::byte, 16> buf{};
    buf[0] = std::byte{static_cast<uint8_t>(PacketType::Audio)};
    auto decoded = aqua::net::decode_hello(buf);
    EXPECT_FALSE(decoded.has_value());
}

// ---- Audio 最大值边界 ----

TEST(PacketTest, AudioMaxSessionIdAndSequence)
{
    std::vector<std::byte> payload(16, std::byte{0xAB});
    std::vector<std::byte> buf(sizeof(AudioPacketHeader) + payload.size());

    auto written = aqua::net::encode_audio(
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, payload, buf);
    ASSERT_GT(written, 0u);

    auto decoded = aqua::net::decode_audio(
        std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.session_id, 0xFFFFFFFFu);
    EXPECT_EQ(decoded->header.sequence, 0xFFFFFFFFu);
    EXPECT_EQ(decoded->header.sample_position, 0xFFFFFFFFu);
}

TEST(PacketTest, AudioPayloadSizeAtU16Max)
{
    // payload_size = 0xFFFF (u16 最大值), 应编码成功
    std::vector<std::byte> payload(0xFFFF, std::byte{0x77});
    std::vector<std::byte> buf(sizeof(AudioPacketHeader) + payload.size());

    auto written = aqua::net::encode_audio(1, 1, 1, payload, buf);
    ASSERT_EQ(written, sizeof(AudioPacketHeader) + 0xFFFF);

    auto decoded = aqua::net::decode_audio(
        std::span<const std::byte>{buf.data(), written});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.payload_size, 0xFFFFu);
    EXPECT_EQ(decoded->payload.size(), 0xFFFFu);
    EXPECT_EQ(decoded->payload.front(), std::byte{0x77});
    EXPECT_EQ(decoded->payload.back(), std::byte{0x77});
}

TEST(PacketTest, AudioWireFormatByteLevel)
{
    // 字节级校验 AudioPacketHeader
    // session_id=0x11223344, sequence=0x55667788, sample_position=0x99AABBCC,
    // payload_size=4
    std::vector<std::byte> payload = {std::byte{0xDE}, std::byte{0xAD},
                                       std::byte{0xBE}, std::byte{0xEF}};
    std::vector<std::byte> buf(sizeof(AudioPacketHeader) + payload.size());

    auto written = aqua::net::encode_audio(0x11223344u, 0x55667788u,
                                            0x99AABBCCu, payload, buf);
    ASSERT_EQ(written, sizeof(AudioPacketHeader) + payload.size());

    EXPECT_EQ(buf[0],  std::byte{0x03}); // Audio type
    EXPECT_EQ(buf[1],  std::byte{0x44}); // session_id LE
    EXPECT_EQ(buf[2],  std::byte{0x33});
    EXPECT_EQ(buf[3],  std::byte{0x22});
    EXPECT_EQ(buf[4],  std::byte{0x11});
    EXPECT_EQ(buf[5],  std::byte{0x88}); // sequence LE
    EXPECT_EQ(buf[6],  std::byte{0x77});
    EXPECT_EQ(buf[7],  std::byte{0x66});
    EXPECT_EQ(buf[8],  std::byte{0x55});
    EXPECT_EQ(buf[9],  std::byte{0xCC}); // sample_position LE
    EXPECT_EQ(buf[10], std::byte{0xBB});
    EXPECT_EQ(buf[11], std::byte{0xAA});
    EXPECT_EQ(buf[12], std::byte{0x99});
    EXPECT_EQ(buf[13], std::byte{0x04}); // payload_size LE = 4
    EXPECT_EQ(buf[14], std::byte{0x00});
    EXPECT_EQ(buf[15], std::byte{0xDE}); // payload
    EXPECT_EQ(buf[16], std::byte{0xAD});
    EXPECT_EQ(buf[17], std::byte{0xBE});
    EXPECT_EQ(buf[18], std::byte{0xEF});
}

TEST(PacketTest, DecodeAudioIgnoresTrailingBytes)
{
    // 包后有多余字节，应正确解码 header + payload，忽略尾部
    std::vector<std::byte> payload(8, std::byte{0x42});
    std::vector<std::byte> buf(sizeof(AudioPacketHeader) + payload.size() + 10);

    auto written = aqua::net::encode_audio(7, 3, 100, payload, buf);
    ASSERT_GT(written, 0u);
    // 喂入包含 trailing 的 span
    auto decoded = aqua::net::decode_audio(
        std::span<const std::byte>{buf.data(), buf.size()});
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.payload_size, 8u);
    EXPECT_EQ(decoded->payload.size(), 8u);
}

TEST(PacketTest, DecodeAudioPayloadSizeZeroButTrailingGarbage)
{
    // payload_size=0 但缓冲后面有垃圾，应只解码 header
    std::vector<std::byte> buf(sizeof(AudioPacketHeader) + 5, std::byte{0xFF});
    buf[0] = std::byte{0x03}; // Audio
    // payload_size = 0
    buf[13] = std::byte{0x00};
    buf[14] = std::byte{0x00};

    auto decoded = aqua::net::decode_audio(buf);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header.payload_size, 0u);
    EXPECT_EQ(decoded->payload.size(), 0u);
}

// ---- peek_type 全覆盖 ----

TEST(PacketTest, PeekTypeAllValidAndInvalid)
{
    EXPECT_EQ(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{1}}),
              PacketType::Hello);
    EXPECT_EQ(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{2}}),
              PacketType::HelloAck);
    EXPECT_EQ(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{3}}),
              PacketType::Audio);
    EXPECT_FALSE(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{0}}).has_value());
    EXPECT_FALSE(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{4}}).has_value());
    EXPECT_FALSE(aqua::net::peek_type(std::array<std::byte, 1>{std::byte{255}}).has_value());
    EXPECT_FALSE(aqua::net::peek_type({}).has_value());
}
