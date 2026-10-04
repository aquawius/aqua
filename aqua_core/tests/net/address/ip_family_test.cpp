#include "aqua/net/address/ip_family.h"

#include <gtest/gtest.h>

#include <asio/ip/address.hpp>

namespace {

using aqua::net::effective_family;
using aqua::net::FamilySet;
using aqua::net::ip_family_name;
using aqua::net::IpFamily;
using aqua::net::normalize_for_display;
using aqua::net::parse_grpc_peer_address;

// ---- effective_family ----

TEST(IpFamilyTest, EffectiveFamilyDistinguishesV4AndV6)
{
    EXPECT_EQ(effective_family(asio::ip::make_address("192.168.1.10")), IpFamily::V4);
    EXPECT_EQ(effective_family(asio::ip::make_address("0.0.0.0")), IpFamily::V4);
    EXPECT_EQ(effective_family(asio::ip::make_address("2001:db8::1")), IpFamily::V6);
    EXPECT_EQ(effective_family(asio::ip::make_address("fd00::1")), IpFamily::V6);
    // 通配 IPv6 仍然是"IPv6 服务的族"，不是 v4
    EXPECT_EQ(effective_family(asio::ip::make_address("::")), IpFamily::V6);
    EXPECT_EQ(effective_family(asio::ip::make_address("::1")), IpFamily::V6);
    // link-local 仍是 v6（zone id 不改变族判定）
    EXPECT_EQ(effective_family(asio::ip::make_address("fe80::1")), IpFamily::V6);
}

TEST(IpFamilyTest, EffectiveFamilyNormalizesIpv4MappedToV4)
{
    // ::ffff:a.b.c.d 在套接字层是 IPv6，但服务的是 IPv4 客户端 —— 必须归一化为 V4，
    // 否则会出现"用 v6 槽服务 v4 客户端"。
    EXPECT_EQ(effective_family(asio::ip::make_address("::ffff:192.168.1.10")), IpFamily::V4);
    EXPECT_EQ(effective_family(asio::ip::make_address("::ffff:0.0.0.0")), IpFamily::V4);
    EXPECT_EQ(effective_family(asio::ip::make_address("::ffff:255.255.255.255")), IpFamily::V4);
}

// ---- normalize_for_display ----

TEST(IpFamilyTest, NormalizeForDisplayUnmapsIpv4Mapped)
{
    EXPECT_EQ(normalize_for_display(asio::ip::make_address("::ffff:192.168.1.10")), "192.168.1.10");
    EXPECT_EQ(normalize_for_display(asio::ip::make_address("192.168.1.10")), "192.168.1.10");
}

TEST(IpFamilyTest, NormalizeForDisplayKeepsNativeV6Untouched)
{
    const auto v6 = asio::ip::make_address("2001:db8::1");
    EXPECT_EQ(normalize_for_display(v6), v6.to_string());
    const auto loopback = asio::ip::make_address("::1");
    EXPECT_EQ(normalize_for_display(loopback), "::1");
}

// ---- FamilySet / ip_family_name ----

TEST(IpFamilyTest, FamilySetReportsContainment)
{
    const FamilySet both { true, true };
    EXPECT_TRUE(both.contains(IpFamily::V4));
    EXPECT_TRUE(both.contains(IpFamily::V6));
    EXPECT_FALSE(both.contains(IpFamily::Unknown));
    EXPECT_FALSE(both.empty());

    const FamilySet v4_only { true, false };
    EXPECT_TRUE(v4_only.contains(IpFamily::V4));
    EXPECT_FALSE(v4_only.contains(IpFamily::V6));

    const FamilySet none { };
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(none.contains(IpFamily::V4));
    EXPECT_FALSE(none.contains(IpFamily::V6));
}

TEST(IpFamilyTest, FamilySetNameIsStableForLogs)
{
    // 注意：不要在 EXPECT_EQ 里直接写 "FamilySet { true, true }" —— 花括号里的逗号
    // 会被预处理器当作宏参数分隔符（EXPECT_EQ 收到 3 个参数，编译报 C2660）。
    const FamilySet both { true, true };
    const FamilySet v4_only { true, false };
    const FamilySet v6_only { false, true };
    const FamilySet none { };
    EXPECT_EQ(both.to_string(), "v4+v6");
    EXPECT_EQ(v4_only.to_string(), "v4");
    EXPECT_EQ(v6_only.to_string(), "v6");
    EXPECT_EQ(none.to_string(), "none");
}

TEST(IpFamilyTest, IpFamilyNames)
{
    EXPECT_STREQ(ip_family_name(IpFamily::V4), "v4");
    EXPECT_STREQ(ip_family_name(IpFamily::V6), "v6");
    EXPECT_STREQ(ip_family_name(IpFamily::Unknown), "unknown");
}

// ---- parse_grpc_peer_address ----

TEST(GrpcPeerAddressTest, ParsesIpv4Form)
{
    const auto peer = parse_grpc_peer_address("ipv4:192.168.1.10:54321");
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->family, IpFamily::V4);
    EXPECT_EQ(peer->port, 54321);
    EXPECT_EQ(peer->address.to_string(), "192.168.1.10");
}

TEST(GrpcPeerAddressTest, ParsesIpv6Form)
{
    const auto peer = parse_grpc_peer_address("ipv6:[2001:db8::1]:50051");
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->family, IpFamily::V6);
    EXPECT_EQ(peer->port, 50051);
    EXPECT_EQ(peer->address.to_string(), "2001:db8::1");

    const auto loopback = parse_grpc_peer_address("ipv6:[::1]:50051");
    ASSERT_TRUE(loopback.has_value());
    EXPECT_EQ(loopback->family, IpFamily::V6);
    EXPECT_EQ(loopback->address.to_string(), "::1");
}

TEST(GrpcPeerAddressTest, NormalizesMappedIpv6PeerToV4)
{
    // gRPC 双栈监听下，v4 客户端可能被报成 ipv6:[::ffff:a.b.c.d]:port。
    // 归一化后族必须是 V4（这条直接决定 Connect 期用哪个槽判定）。
    const auto peer = parse_grpc_peer_address("ipv6:[::ffff:192.168.1.10]:5000");
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->family, IpFamily::V4);
    EXPECT_EQ(peer->port, 5000);
    EXPECT_EQ(normalize_for_display(peer->address), "192.168.1.10");
}

TEST(GrpcPeerAddressTest, ReturnsNulloptForUndeterminablePeers)
{
    // 无法判定 => nullopt => 调用方必须按"未知"处理（不得据此拒绝客户端）。
    EXPECT_FALSE(parse_grpc_peer_address("").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("unix:/tmp/aqua.sock").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("unix-abstract:aqua").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("192.168.1.10:5000").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:192.168.1.10").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:192.168.1.10:").has_value());
    // v6 前缀必须带方括号
    EXPECT_FALSE(parse_grpc_peer_address("ipv6:192.168.1.10:5000").has_value());
    // 空地址
    EXPECT_FALSE(parse_grpc_peer_address("ipv6:[]:5000").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4::5000").has_value());
    // 非法地址 / 非法端口
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:999.1.1.1:5000").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:192.168.1.10:abc").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:192.168.1.10:65536").has_value());
    EXPECT_FALSE(parse_grpc_peer_address("ipv4:192.168.1.10: 5000").has_value());
}

} // namespace
