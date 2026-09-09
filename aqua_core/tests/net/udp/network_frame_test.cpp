#include "aqua/net/udp/network_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

using aqua::net::NetworkFrame;
using aqua::net::PacketType;

TEST(NetworkFrameTest, AudioRoundTrip)
{
    std::array<std::byte, 16> pcm { };
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::byte>(i);
    }
    const auto packet = NetworkFrame::audio(0xBEEFu, 0x12345678u, 0xA5A5A5A5u, pcm).encode();

    EXPECT_EQ(packet.size(), aqua::net::kRtpHeaderBytes + 16u);
    // RTP 头大端布局断言：V=2 / M=0+PT=96 / seq / timestamp / SSRC。
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[0]), 0x80u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[1]), 96u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[2]), 0xBEu);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[3]), 0xEFu);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[4]), 0x12u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[5]), 0x34u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[6]), 0x56u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[7]), 0x78u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[8]), 0xA5u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(packet[11]), 0xA5u);

    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type(), PacketType::Audio);
    EXPECT_EQ(decoded->payload_type(), 96u);
    EXPECT_EQ(decoded->rtp_sequence(), 0xBEEFu);
    EXPECT_EQ(decoded->timestamp(), 0x12345678u);
    EXPECT_EQ(decoded->ssrc(), 0xA5A5A5A5u);
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
    EXPECT_FALSE(NetworkFrame::decode(std::span<const std::byte> { }).has_value());

    const std::array<std::byte, 1> unknown { std::byte { 0x7F } };
    EXPECT_FALSE(NetworkFrame::decode(unknown).has_value());
}

TEST(NetworkFrameTest, DecodeRejectsBadRtpHeader)
{
    std::array<std::byte, 16> wire { };
    wire[0] = std::byte { 0x80 };
    wire[1] = std::byte { 96 };
    // M 位置位 → 非法（不断流语义）。
    wire[1] = std::byte { 0x80 | 96 };
    EXPECT_FALSE(NetworkFrame::decode(wire).has_value());
    // PT 非 96 → 非法（Opus 预留 97，后续版本）。
    wire[1] = std::byte { 97 };
    EXPECT_FALSE(NetworkFrame::decode(wire).has_value());
    // 版本/P/X/CC 非 0x80 → 走控制面分支，长度不符 → 非法。
    wire[0] = std::byte { 0x00 };
    wire[1] = std::byte { 96 };
    EXPECT_FALSE(NetworkFrame::decode(wire).has_value());
}

TEST(NetworkFrameTest, DecodeAudioRejectsShort)
{
    // 11 字节（缺 1 字节头）→ 解码失败。
    const std::array<std::byte, 11> short_packet { std::byte { 0x80 } };
    EXPECT_FALSE(NetworkFrame::decode(short_packet).has_value());
}

TEST(NetworkFrameTest, DecodeHelloRejectsShort)
{
    const std::array<std::byte, 4> short_packet { };
    EXPECT_FALSE(NetworkFrame::decode(short_packet).has_value());
}

TEST(NetworkFrameTest, PayloadIsViewNotCopy)
{
    const std::array<std::byte, 4> pcm { std::byte { 1 }, std::byte { 2 }, std::byte { 3 },
        std::byte { 4 } };
    auto packet = NetworkFrame::audio(7, 700, 0x01020304u, pcm).encode();

    const auto decoded = NetworkFrame::decode(packet);
    ASSERT_TRUE(decoded.has_value());
    // payload 应指向 packet 内部（12B RTP 头之后），而非独立拷贝。
    EXPECT_EQ(decoded->payload().data(), packet.data() + 12);
    EXPECT_EQ(std::to_integer<std::uint8_t>(decoded->payload()[2]), 3u);
}

TEST(NetworkFrameTest, ExtendRtpSequence)
{
    using aqua::net::extend_rtp_sequence;
    // 首包直接采用。
    EXPECT_EQ(extend_rtp_sequence(std::nullopt, 7), 7u);
    // 顺序递增。
    EXPECT_EQ(extend_rtp_sequence(7u, 8), 8u);
    // 小幅乱序回退（迟到包）。
    EXPECT_EQ(extend_rtp_sequence(100u, 99), 99u);
    // 前向回绕：65535 → 0。
    EXPECT_EQ(extend_rtp_sequence(65535u, 0), 65536u);
    EXPECT_EQ(extend_rtp_sequence(65536u, 1), 65537u);
    // 回绕边界附近的迟到包：已越过回绕点后收到回绕前包。
    EXPECT_EQ(extend_rtp_sequence(65636u, 65530), 65530u);
    // 大跨度前跳（半窗内）：按前向处理。
    EXPECT_EQ(extend_rtp_sequence(0u, 40000), 40000u);
}

} // namespace
