#include <gtest/gtest.h>

#include "core/logger/logger.h"

TEST(LogTest, SetLevelAndLog)
{
    aqua::set_log_level(aqua::LogLevel::Warn);

    aqua::log_trace("trace message");
    aqua::log_debug("debug message");
    aqua::log_info("info message");
    aqua::log_warn("warn message");
    aqua::log_error("error message");

    aqua::log_info_fmt("formatted {}", "info");
    aqua::log_error_fmt("formatted {}", "error");

    SUCCEED();
}

TEST(LogTest, AllLevelsCanBeSet)
{
    aqua::set_log_level(aqua::LogLevel::Trace);
    aqua::log_trace("trace");
    aqua::set_log_level(aqua::LogLevel::Debug);
    aqua::log_debug("debug");
    aqua::set_log_level(aqua::LogLevel::Info);
    aqua::log_info("info");
    aqua::set_log_level(aqua::LogLevel::Warn);
    aqua::log_warn("warn");
    aqua::set_log_level(aqua::LogLevel::Error);
    aqua::log_error("error");
    SUCCEED();
}

TEST(LogTest, FormattedLogDoesNotThrow)
{
    aqua::set_log_level(aqua::LogLevel::Info);
    aqua::log_info_fmt("test {} {}", 42, 3.14);
    aqua::log_warn_fmt("warning: {}", "test");
    aqua::log_error_fmt("error: {}", "test");
    SUCCEED();
}

TEST(LogTest, LogLevelFromStringValid)
{
    EXPECT_EQ(aqua::log_level_from_string("trace"), aqua::LogLevel::Trace);
    EXPECT_EQ(aqua::log_level_from_string("debug"), aqua::LogLevel::Debug);
    EXPECT_EQ(aqua::log_level_from_string("info"), aqua::LogLevel::Info);
    EXPECT_EQ(aqua::log_level_from_string("warn"), aqua::LogLevel::Warn);
    EXPECT_EQ(aqua::log_level_from_string("error"), aqua::LogLevel::Error);
}

TEST(LogTest, LogLevelFromStringCaseInsensitive)
{
    EXPECT_EQ(aqua::log_level_from_string("TRACE"), aqua::LogLevel::Trace);
    EXPECT_EQ(aqua::log_level_from_string("Info"), aqua::LogLevel::Info);
    EXPECT_EQ(aqua::log_level_from_string("WARN"), aqua::LogLevel::Warn);
}

TEST(LogTest, LogLevelFromStringInvalid)
{
    EXPECT_FALSE(aqua::log_level_from_string("verbose").has_value());
    EXPECT_FALSE(aqua::log_level_from_string("").has_value());
    EXPECT_FALSE(aqua::log_level_from_string("fatal").has_value());
}
