#include "core/logger/logger.h"

namespace aqua {
namespace {
    spdlog::level::level_enum to_spdlog(LogLevel level)
    {
        switch (level) {
        case LogLevel::Trace:
            return spdlog::level::trace;
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
        }
        return spdlog::level::info;
    }
} // namespace

LogLevel default_log_level()
{
#ifdef AQUA_DEBUG
    return LogLevel::Debug;
#else
    return LogLevel::Info;
#endif
}

void set_log_level(LogLevel level)
{
    spdlog::set_level(to_spdlog(level));
}

void log_trace(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::trace, message); }
void log_debug(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::debug, message); }
void log_info(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::info, message); }
void log_warn(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::warn, message); }
void log_error(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::err, message); }

} // namespace aqua
