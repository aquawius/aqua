#include "aqua/net/address/address_utils.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using aqua::net::format_host_port;
using aqua::net::parse_ip_address;

TEST(AddressUtilsTest, ParsesIPv4Literal)
{
    const auto address = parse_ip_address("192.168.1.10");
    EXPECT_TRUE(address.is_v4());
    EXPECT_EQ(address.to_string(), "192.168.1.10");
}

TEST(AddressUtilsTest, ParsesIPv6Literal)
{
    const auto address = parse_ip_address("2001:db8::10");
    EXPECT_TRUE(address.is_v6());
    EXPECT_EQ(address.to_string(), "2001:db8::10");
}

TEST(AddressUtilsTest, ParsesBracketedIPv6Literal)
{
    const auto address = parse_ip_address("[2001:db8::10]");
    EXPECT_TRUE(address.is_v6());
    EXPECT_EQ(address.to_string(), "2001:db8::10");
}

TEST(AddressUtilsTest, PreservesIPv6ZoneIdentifier)
{
    const auto address = parse_ip_address("fe80::1%1");
    EXPECT_TRUE(address.is_v6());
}

TEST(AddressUtilsTest, RejectsEmptyAddress)
{
    EXPECT_THROW(parse_ip_address(""), std::invalid_argument);
}

TEST(AddressUtilsTest, RejectsHostNames)
{
    EXPECT_THROW(parse_ip_address("aqua-server.local"), std::exception);
    // format_host_port 永不抛异常：非 IP 输入退化为原始 host:port（日志用）。
    EXPECT_EQ(format_host_port("aqua-server.local", 50051), "aqua-server.local:50051");
}

TEST(AddressUtilsTest, FormatsIPv4HostPort)
{
    EXPECT_EQ(format_host_port("192.168.1.10", 50051), "192.168.1.10:50051");
}

TEST(AddressUtilsTest, FormatsIPv6HostPortWithBrackets)
{
    EXPECT_EQ(format_host_port("2001:db8::10", 50051), "[2001:db8::10]:50051");
    EXPECT_EQ(format_host_port("[2001:db8::10]", 50051), "[2001:db8::10]:50051");
}

TEST(AddressUtilsTest, FormatsZeroPort)
{
    EXPECT_EQ(format_host_port("127.0.0.1", 0), "127.0.0.1:0");
}

} // namespace
