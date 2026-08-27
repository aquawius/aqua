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

TEST(NetworkFrameBoundaryTest, EmptyAudioPayloadIsRejected)
{
    const auto pkt = NetworkFrame::audio(5, std::span<const std::byte> {}).encode();
    EXPECT_EQ(pkt.size(), 9u); // encode 仍保留完整 header；decode 拒绝空 payload。
    EXPECT_FALSE(NetworkFrame::decode(pkt).has_value());
}

TEST(NetworkFrameBoundaryTest, ExactHeaderLengthBoundary)
{
    // 8 字节（缺 1 字节 sequence）→ 解码失败。
    std::array<std::byte, 8> short_pkt {};
    short_pkt[0] = static_cast<std::byte>(PacketType::Audio);
    EXPECT_FALSE(NetworkFrame::decode(short_pkt).has_value());

    // 9 字节（恰好 header）→ 由于 Audio 不允许空 payload，仍应拒绝。
    std::array<std::byte, 9> exact {};
    exact[0] = static_cast<std::byte>(PacketType::Audio);
    EXPECT_FALSE(NetworkFrame::decode(exact).has_value());
}

TEST(NetworkFrameBoundaryTest, UnknownTypeByte)
{
    const std::array<std::byte, 1> unknown { std::byte { 0xFF } };
    EXPECT_FALSE(NetworkFrame::decode(unknown).has_value());
}

} // namespace
