#ifndef AQUA_LOGGER_H
#define AQUA_LOGGER_H

#include <spdlog/spdlog.h>
#include <string_view>

namespace aqua {
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

void set_log_level(LogLevel level);

void log_trace(std::string_view message);
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

template <typename... Args>
void log_trace_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    spdlog::default_logger_raw()->log(spdlog::level::trace, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_debug_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    spdlog::default_logger_raw()->log(spdlog::level::debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_info_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    spdlog::default_logger_raw()->log(spdlog::level::info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_warn_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    spdlog::default_logger_raw()->log(spdlog::level::warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void log_error_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args)
{
    spdlog::default_logger_raw()->log(spdlog::level::err, fmt, std::forward<Args>(args)...);
}
} // namespace aqua

#endif // AQUA_LOGGER_H
