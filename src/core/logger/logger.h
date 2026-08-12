#ifndef AQUA_LOGGER_H
#define AQUA_LOGGER_H

#include <spdlog/spdlog.h>
#include <optional>
#include <string_view>

namespace aqua {
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

// 编译期默认日志级别。
// Debug 构建（CMake 选项 AQUA_DEBUG=ON，定义 AQUA_DEBUG 宏）返回 Debug；
// 否则返回 Info。由 main 在启动时调用 set_log_level(default_log_level())。
LogLevel default_log_level();

// 从字符串解析日志等级（大小写不敏感）。
// 接受 "trace"/"debug"/"info"/"warn"/"error"，返回 std::nullopt 表示无效输入。
std::optional<LogLevel> log_level_from_string(std::string_view name);

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
