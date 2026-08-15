#include <gtest/gtest.h>

#include "app/cli/cli_parser_client.h"

TEST(CliParserClientTest, Defaults)
{
    auto parsed = aqua::parse_client_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.server_ip, "127.0.0.1");
    EXPECT_EQ(parsed.server_rpc_port, 50051);
    EXPECT_EQ(parsed.jitter_latency_ms, 0);
}

TEST(CliParserClientTest, CustomOptions)
{
    auto parsed = aqua::parse_client_command_line(
        {"--server-ip", "192.168.1.100", "--server-rpc-port", "60000", "--jitter-latency", "18"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.server_ip, "192.168.1.100");
    EXPECT_EQ(parsed.server_rpc_port, 60000);
    EXPECT_EQ(parsed.jitter_latency_ms, 18);
}

TEST(CliParserClientTest, AutoReconnectFlag)
{
    // 默认关闭
    auto def = aqua::parse_client_command_line({});
    ASSERT_TRUE(def.success);
    EXPECT_FALSE(def.auto_reconnect);

    // 显式开启
    auto on = aqua::parse_client_command_line({"--auto-reconnect"});
    ASSERT_TRUE(on.success);
    EXPECT_TRUE(on.auto_reconnect);
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

TEST(CliParserClientTest, RejectsPositionalArgument)
{
    auto parsed = aqua::parse_client_command_line({"192.168.1.100"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
    EXPECT_NE(parsed.error_message.find("--help"), std::string::npos);
}

TEST(CliParserClientTest, RejectsUnknownOption)
{
    auto parsed = aqua::parse_client_command_line({"--foo", "bar"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
    EXPECT_NE(parsed.error_message.find("--help"), std::string::npos);
}

// ---- 新增选项测试 ----

TEST(CliParserClientTest, DriftThresholdOption)
{
    // 默认值 0
    auto parsed = aqua::parse_client_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.drift_late_threshold, 0u);

    // 自定义值
    parsed = aqua::parse_client_command_line({"--drift-threshold", "30"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.drift_late_threshold, 30u);
}

TEST(CliParserClientTest, DriftThresholdRejectsNegative)
{
    auto parsed = aqua::parse_client_command_line({"--drift-threshold", "-1"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserClientTest, DriftThresholdRejectsTooLarge)
{
    auto parsed = aqua::parse_client_command_line({"--drift-threshold", "10001"});
    EXPECT_FALSE(parsed.success);
    EXPECT_NE(parsed.error_message.find("0..10000"), std::string::npos);
}

TEST(CliParserClientTest, PlaybackBufferOption)
{
    // 默认值 0
    auto parsed = aqua::parse_client_command_line({});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.playback_buffer_size, 0u);

    // 自定义值
    parsed = aqua::parse_client_command_line({"--playback-buffer", "32768"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.playback_buffer_size, 32768u);
}

TEST(CliParserClientTest, PlaybackBufferRejectsNegative)
{
    auto parsed = aqua::parse_client_command_line({"--playback-buffer", "-1"});
    EXPECT_FALSE(parsed.success);
    EXPECT_FALSE(parsed.error_message.empty());
}

TEST(CliParserClientTest, PlaybackBufferRejectsTooLarge)
{
    auto parsed = aqua::parse_client_command_line({"--playback-buffer", "67108865"});
    EXPECT_FALSE(parsed.success);
    EXPECT_NE(parsed.error_message.find("64MB"), std::string::npos);
}

TEST(CliParserClientTest, LogLevelOption)
{
    // trace
    auto parsed = aqua::parse_client_command_line({"--log-level", "trace"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.log_level, aqua::LogLevel::Trace);

    // info
    parsed = aqua::parse_client_command_line({"--log-level", "info"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.log_level, aqua::LogLevel::Info);

    // warn (short form -l)
    parsed = aqua::parse_client_command_line({"-l", "warn"});
    ASSERT_TRUE(parsed.success);
    EXPECT_EQ(parsed.log_level, aqua::LogLevel::Warn);
}

TEST(CliParserClientTest, LogLevelRejectsInvalid)
{
    auto parsed = aqua::parse_client_command_line({"--log-level", "verbose"});
    EXPECT_FALSE(parsed.success);
    EXPECT_NE(parsed.error_message.find("Invalid --log-level"), std::string::npos);
}
