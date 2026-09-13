#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_config.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using aqua::net::NetworkFrame;
using aqua::net::PacketType;

TEST(NetworkFrameBoundaryTest, RtpFieldExtremes)
{
    const std::array<std::byte, 1> payload { std::byte { 0x7F } };
    for (const auto seq : { std::uint16_t { 0 }, std::numeric_limits<std::uint16_t>::max() }) {
        for (const auto ts : { std::uint32_t { 0 }, std::numeric_limits<std::uint32_t>::max() }) {
            const auto pkt = NetworkFrame::audio(seq, ts, 0xDEADBEEFu, payload).encode();
            const auto decoded = NetworkFrame::decode(pkt);
            ASSERT_TRUE(decoded.has_value());
            EXPECT_EQ(decoded->rtp_sequence(), seq);
            EXPECT_EQ(decoded->timestamp(), ts);
            EXPECT_EQ(decoded->ssrc(), 0xDEADBEEFu);
        }
    }
}

TEST(NetworkFrameBoundaryTest, SessionIdExtremes)
{
    for (const auto sid : { 0u, std::numeric_limits<std::uint32_t>::max() }) {
        const auto pkt = NetworkFrame::heartbeat(sid).encode();
        const auto decoded = NetworkFrame::decode(pkt);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->session_id(), sid);
    }
}

TEST(NetworkFrameBoundaryTest, EmptyAudioPayloadIsRejected)
{
    const auto pkt = NetworkFrame::audio(5, 500, 1u, std::span<const std::byte> { }).encode();
    EXPECT_TRUE(pkt.empty());
    EXPECT_FALSE(NetworkFrame::decode(pkt).has_value());
}

TEST(NetworkFrameBoundaryTest, ExactHeaderLengthBoundary)
{
    // 11 字节（缺 1 字节 RTP 头）→ 解码失败。
    std::array<std::byte, 11> short_pkt { };
    short_pkt[0] = std::byte { 0x80 };
    EXPECT_FALSE(NetworkFrame::decode(short_pkt).has_value());

    // 12 字节（恰好 RTP 头）→ 由于 Audio 不允许空 payload，仍应拒绝。
    std::array<std::byte, 12> exact { };
    exact[0] = std::byte { 0x80 };
    exact[1] = std::byte { 96 };
    EXPECT_FALSE(NetworkFrame::decode(exact).has_value());
}

TEST(NetworkFrameBoundaryTest, ExactMaximumAudioPayloadIsAccepted)
{
    std::vector<std::byte> payload(aqua::config::UDP_AUDIO_PAYLOAD_BYTES, std::byte { 0x5A });
    EXPECT_EQ(aqua::config::UDP_AUDIO_PAYLOAD_BYTES, 1400u); // IPv6 1440 再让 40B 隧道/封装余量
    const auto pkt = NetworkFrame::audio(123, 12300, 7u, payload).encode();
    ASSERT_EQ(pkt.size(), aqua::net::kRtpHeaderBytes + payload.size());

    const auto decoded = NetworkFrame::decode(pkt);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->rtp_sequence(), 123u);
    EXPECT_EQ(decoded->payload().size(), aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
}

TEST(NetworkFrameBoundaryTest, OversizedAudioPayloadIsRejected)
{
    std::vector<std::byte> oversized(aqua::config::UDP_AUDIO_PAYLOAD_BYTES + 1);
    const auto pkt = NetworkFrame::audio(1, 100, 1u, oversized).encode();
    EXPECT_TRUE(pkt.empty());

    std::vector<std::byte> wire(aqua::net::kRtpHeaderBytes
            + aqua::config::UDP_AUDIO_PAYLOAD_BYTES + 1,
        std::byte { 0 });
    wire[0] = std::byte { 0x80 };
    wire[1] = std::byte { 96 };
    EXPECT_FALSE(NetworkFrame::decode(wire).has_value());
}

TEST(NetworkFrameBoundaryTest, UnknownTypeByte)
{
    const std::array<std::byte, 1> unknown { std::byte { 0xFF } };
    EXPECT_FALSE(NetworkFrame::decode(unknown).has_value());
}

} // namespace
