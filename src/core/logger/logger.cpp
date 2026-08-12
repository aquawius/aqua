#include "core/logger/logger.h"

#include <cstring>

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

std::optional<LogLevel> log_level_from_string(std::string_view name)
{
    // 大小写不敏感比较
    auto eq = [](std::string_view a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
            if (ca != b[i]) return false;
        }
        return true;
    };
    if (eq(name, "trace")) return LogLevel::Trace;
    if (eq(name, "debug")) return LogLevel::Debug;
    if (eq(name, "info"))  return LogLevel::Info;
    if (eq(name, "warn"))  return LogLevel::Warn;
    if (eq(name, "error")) return LogLevel::Error;
    return std::nullopt;
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
