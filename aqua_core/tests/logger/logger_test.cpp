#include <gtest/gtest.h>

#include "aqua/logger/logger.h"

TEST(LogTest, DefaultLevelAndParsing)
{
    EXPECT_EQ(aqua::default_log_level(), aqua::LogLevel::Info);

    const auto fatal = aqua::string_to_log_level_enum("fatal");
    ASSERT_TRUE(fatal.has_value());
    EXPECT_EQ(*fatal, aqua::LogLevel::Fatal);
    EXPECT_EQ(*aqua::string_to_log_level_enum("critical"), aqua::LogLevel::Fatal);
    EXPECT_EQ(*aqua::string_to_log_level_enum("warning"), aqua::LogLevel::Warn);
    EXPECT_EQ(*aqua::string_to_log_level_enum("warn"), aqua::LogLevel::Warn);
    EXPECT_FALSE(aqua::string_to_log_level_enum("verbose").has_value());
}

TEST(LogTest, LogLevelNames)
{
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Trace), "trace");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Debug), "debug");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Info), "info");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Warn), "warn");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Error), "error");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Fatal), "fatal");
}

TEST(LogTest, SetLevelAndLog)
{
    aqua::set_log_level(aqua::LogLevel::Warn);

    EXPECT_FALSE(aqua::log_level_enabled(aqua::LogLevel::Debug));
    EXPECT_TRUE(aqua::log_level_enabled(aqua::LogLevel::Warn));
    EXPECT_TRUE(aqua::log_level_enabled(aqua::LogLevel::Fatal));

    aqua::log_trace("trace message");
    aqua::log_debug("debug message");
    aqua::log_info("info message");
    aqua::log_warn("warn message");
    aqua::log_error("error message");
    aqua::log_fatal("fatal message");

    aqua::log_info_fmt("formatted {}", "info");
    aqua::log_error_fmt("formatted {}", "error");

    aqua::set_log_level(aqua::LogLevel::Info);
}
