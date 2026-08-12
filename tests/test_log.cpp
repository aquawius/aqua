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
