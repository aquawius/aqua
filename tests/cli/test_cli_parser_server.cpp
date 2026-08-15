#include <gtest/gtest.h>

#include "app/cli/cli_parser_server.h"

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

// ---- 新增选项测试 ----

TEST(CliParserServerTest, CaptureBufferOption)
{
    // 默认值 0
    auto parsed = aqua::parse_server_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.capture_buffer_size, 0u);

    // 自定义值
    parsed = aqua::parse_server_command_line({"--capture-buffer", "16384"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.capture_buffer_size, 16384u);
}

TEST(CliParserServerTest, CaptureBufferRejectsNegative)
{
    auto parsed = aqua::parse_server_command_line({"--capture-buffer", "-1"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserServerTest, CaptureBufferRejectsTooLarge)
{
    auto parsed = aqua::parse_server_command_line({"--capture-buffer", "67108865"});
    EXPECT_FALSE(parsed.success);
    EXPECT_NE(parsed.error_message.find("64MB"), std::string::npos);
}

TEST(CliParserServerTest, LogLevelOption)
{
    // trace
    auto parsed = aqua::parse_server_command_line({"--log-level", "trace"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.log_level, aqua::LogLevel::Trace);

    // error (short form -l)
    parsed = aqua::parse_server_command_line({"-l", "error"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.log_level, aqua::LogLevel::Error);
}

TEST(CliParserServerTest, LogLevelRejectsInvalid)
{
    auto parsed = aqua::parse_server_command_line({"--log-level", "verbose"});
    EXPECT_FALSE(parsed.success);
    EXPECT_NE(parsed.error_message.find("Invalid --log-level"), std::string::npos);
}
