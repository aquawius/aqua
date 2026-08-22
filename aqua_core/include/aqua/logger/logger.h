#ifndef AQUA_LOGGER_H
#define AQUA_LOGGER_H

#include <optional>
#include <spdlog/spdlog.h>
#include <string_view>

namespace aqua {
    // 日志统一走 spdlog 默认 logger。sink 由 init_logger() 按平台选好：
    //   - Android：换成 logcat sink（tag=aqua），因为 app 进程的 stdout 指向 /dev/null；
    //   - Windows 等其他平台：spdlog 默认的 stdout 彩色 sink。
    // pattern 均为 spdlog 默认格式。main 启动时先调用 init_logger()，
    // 再调用 set_log_level(default_log_level()) 设定级别。

    enum class LogLevel {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
    };

    // 初始化日志系统：把 spdlog 默认 logger 替换为当前平台的输出 sink
    // （Android 为 logcat sink，其余平台为 stdout 彩色 sink），pattern 用默认格式。
    // 必须在任何 log_* 调用之前由 main 启动时调用一次。
    void init_logger();

    // 编译期默认日志级别。
    // Debug 构建（CMake 选项 AQUA_DEBUG=ON，定义 AQUA_DEBUG 宏）返回 Debug；
    // 否则返回 Info。由 main 在启动时调用 set_log_level(default_log_level())。
    LogLevel default_log_level();

    // 接受 "trace"/"debug"/"info"/"warn"/"error"，返回 std::nullopt 表示无效输入。
    std::optional<LogLevel> string_to_log_level_enum(std::string_view name);

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
