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