#include <gtest/gtest.h>

#include "core/cli_main/cli_parser_server.h"

TEST(CliParserServerTest, Defaults)
{
    auto parsed = aqua::parse_server_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.bind_ip, "0.0.0.0");
    EXPECT_EQ(parsed.rpc_port, 50051);
    EXPECT_EQ(parsed.udp_port, 50000);
}

TEST(CliParserServerTest, CustomOptions)
{
    auto parsed = aqua::parse_server_command_line(
        {"--bind-ip", "192.168.1.1", "--rpc-port", "60000", "--udp-port", "60001"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.bind_ip, "192.168.1.1");
    EXPECT_EQ(parsed.rpc_port, 60000);
    EXPECT_EQ(parsed.udp_port, 60001);
}

TEST(CliParserServerTest, Help)
{
    auto parsed = aqua::parse_server_command_line({"--help"});
    ASSERT_TRUE(parsed.success);
    EXPECT_TRUE(parsed.show_help);
    EXPECT_FALSE(parsed.help_message.empty());
}

TEST(CliParserServerTest, Version)
{
    auto parsed = aqua::parse_server_command_line({"--version"});
    ASSERT_TRUE(parsed.success);
    EXPECT_TRUE(parsed.show_version);
}

TEST(CliParserServerTest, InvalidPort)
{
    auto parsed = aqua::parse_server_command_line({"--rpc-port", "70000"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserServerTest, NonNumericPort)
{
    auto parsed = aqua::parse_server_command_line({"--udp-port", "abc"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserServerTest, RejectsPositionalArgument)
{
    // 裸位置参数（如 "aqua_server 192.168.45.1"）应被拒绝
    auto parsed = aqua::parse_server_command_line({"192.168.45.1"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
    EXPECT_NE(parsed.error_message.find("--help"), std::string::npos);
}

TEST(CliParserServerTest, RejectsUnknownOption)
{
    auto parsed = aqua::parse_server_command_line({"--foo", "bar"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
    EXPECT_NE(parsed.error_message.find("--help"), std::string::npos);
}
