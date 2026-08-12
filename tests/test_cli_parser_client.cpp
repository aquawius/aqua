#include <gtest/gtest.h>

#include "../src/main/cli_parser_client.h"

TEST(CliParserClientTest, Defaults)
{
    auto parsed = aqua::parse_client_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.server_ip, "127.0.0.1");
    EXPECT_EQ(parsed.server_rpc_port, 50051);
    EXPECT_EQ(parsed.server_udp_port, 50000);
}

TEST(CliParserClientTest, CustomOptions)
{
    auto parsed = aqua::parse_client_command_line(
        {"--server-ip", "192.168.1.100", "--server-rpc-port", "60000", "--server-udp-port", "60001"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.server_ip, "192.168.1.100");
    EXPECT_EQ(parsed.server_rpc_port, 60000);
    EXPECT_EQ(parsed.server_udp_port, 60001);
}

TEST(CliParserClientTest, Help)
{
    auto parsed = aqua::parse_client_command_line({"--help"});
    ASSERT_TRUE(parsed.success);
    EXPECT_TRUE(parsed.show_help);
    EXPECT_FALSE(parsed.help_message.empty());
}

TEST(CliParserClientTest, Version)
{
    auto parsed = aqua::parse_client_command_line({"--version"});
    ASSERT_TRUE(parsed.success);
    EXPECT_TRUE(parsed.show_version);
}

TEST(CliParserClientTest, InvalidPort)
{
    auto parsed = aqua::parse_client_command_line({"--server-rpc-port", "70000"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserClientTest, NonNumericPort)
{
    auto parsed = aqua::parse_client_command_line({"--server-rpc-port", "abc"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}
