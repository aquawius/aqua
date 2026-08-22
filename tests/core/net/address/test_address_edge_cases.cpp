#include "core/net/address/address_utils.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using aqua::net::format_host_port;
using aqua::net::parse_ip_address;

TEST(AddressUtilsEdgeTest, RejectsMalformedBracketedIPv6)
{
    EXPECT_THROW(parse_ip_address("[2001:db8::1"), std::exception);
    EXPECT_THROW(parse_ip_address("2001:db8::1]"), std::exception);
    EXPECT_THROW(parse_ip_address("[]"), std::exception);
}

TEST(AddressUtilsEdgeTest, PreservesIpv4MappedIpv6AsIpv6)
{
    const auto address = parse_ip_address("::ffff:192.0.2.1");
    EXPECT_TRUE(address.is_v6());
    EXPECT_EQ(address.to_string(), "::ffff:192.0.2.1");
}

TEST(AddressUtilsEdgeTest, FormatsIpv6ZoneIdentifierWithBrackets)
{
    const auto address = parse_ip_address("fe80::1%1");
    ASSERT_TRUE(address.is_v6());
    EXPECT_EQ(format_host_port(address.to_string(), 9999),
        "[fe80::1%1]:9999");
}

TEST(AddressUtilsEdgeTest, AcceptsBracketedIpv6ZoneIdentifier)
{
    const auto address = parse_ip_address("[fe80::1%1]");
    EXPECT_TRUE(address.is_v6());
    EXPECT_EQ(address.to_string(), "fe80::1%1");
}

} // namespace
