#include "aqua/net/udp/network_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

using aqua::net::NetworkFrame;
using aqua::net::PacketType;

TEST(NetworkFrameBoundaryTest, SequenceExtremes)
{
    for (const auto seq : { 0ULL, std::numeric_limits<std::uint64_t>::max() }) {
        const auto pkt = NetworkFrame::audio(seq, std::span<const std::byte> {}).encode();
        const auto decoded = NetworkFrame::decode(pkt);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->sequence(), seq);
    }
}

TEST(NetworkFrameBoundaryTest, SessionIdExtremes)
{
    for (const auto sid : { 0u, std::numeric_limits<std::uint32_t>::max() }) {
        const auto pkt = NetworkFrame::hello(sid).encode();
        const auto decoded = NetworkFrame::decode(pkt);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->session_id(), sid);
    }
}

TEST(NetworkFrameBoundaryTest, EmptyAudioPayload)
{
    const auto pkt = NetworkFrame::audio(5, std::span<const std::byte> {}).encode();
    EXPECT_EQ(pkt.size(), 9u); // 仅 header
    const auto decoded = NetworkFrame::decode(pkt);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->sequence(), 5u);
    EXPECT_EQ(decoded->payload().size(), 0u);
}

TEST(NetworkFrameBoundaryTest, ExactHeaderLengthBoundary)
{
    // 8 字节（缺 1 字节 sequence）→ 解码失败。
    std::array<std::byte, 8> short_pkt {};
    short_pkt[0] = static_cast<std::byte>(PacketType::Audio);
    EXPECT_FALSE(NetworkFrame::decode(short_pkt).has_value());

    // 9 字节（恰好 header）→ 解码成功（空 payload）。
    std::array<std::byte, 9> exact {};
    exact[0] = static_cast<std::byte>(PacketType::Audio);
    const auto decoded = NetworkFrame::decode(exact);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->payload().size(), 0u);
}

TEST(NetworkFrameBoundaryTest, UnknownTypeByte)
{
    const std::array<std::byte, 1> unknown { std::byte { 0xFF } };
    EXPECT_FALSE(NetworkFrame::decode(unknown).has_value());
}

} // namespace
