#include "aqua/logger/logger.h"

#include <memory>

#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

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
        case LogLevel::Fatal:
            return spdlog::level::critical;
        }
        return spdlog::level::info;
    }

} // namespace

void init_logger()
{
    // 把 spdlog 默认 logger 替换为当前平台的输出 sink，保证日志在
    // Windows / Android 上都能输出到正确目的地（pattern 保持 spdlog 默认格式）：
    //   - Android：app 进程的 stdout/stderr 指向 /dev/null，默认 stdout sink 的
    //     输出会全部丢失（adb 与 Android Studio 的 logcat 都看不到 native 日志），
    //     因此换成 logcat sink（tag=aqua）。
    //   - 其他平台（Windows 等）：使用 spdlog 默认的 stdout 彩色 sink。
    // 默认级别统一为 info；Debug/Trace 由应用层显式选择。
    std::shared_ptr<spdlog::logger> logger;
#ifdef __ANDROID__
    logger = std::make_shared<spdlog::logger>(
        "aqua", std::make_shared<spdlog::sinks::android_sink_mt>("aqua"));
#else
    logger = std::make_shared<spdlog::logger>(
        "aqua", std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

LogLevel default_log_level()
{
    return LogLevel::Info;
}

const char* log_level_name(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Fatal: return "fatal";
    }
    return "info";
}
std::optional<LogLevel> string_to_log_level_enum(std::string_view name)
{
    if (name == "trace")
        return LogLevel::Trace;
    if (name == "debug")
        return LogLevel::Debug;
    if (name == "info")
        return LogLevel::Info;
    if (name == "warn" || name == "warning")
        return LogLevel::Warn;
    if (name == "error")
        return LogLevel::Error;
    if (name == "fatal" || name == "critical")
        return LogLevel::Fatal;

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
void log_fatal(std::string_view message) { spdlog::default_logger_raw()->log(spdlog::level::critical, message); }

bool log_level_enabled(LogLevel level) noexcept
{
    return spdlog::default_logger_raw()->should_log(to_spdlog(level));
}

} // namespace aqua
