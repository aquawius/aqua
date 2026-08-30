#include "aqua/logger/logger.h"

#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace aqua {

namespace {
    bool is_valid_utf8(std::string_view value) noexcept
    {
        std::size_t i = 0;
        while (i < value.size()) {
            const auto byte = static_cast<unsigned char>(value[i]);
            std::size_t needed = 0;
            if (byte <= 0x7F) {
                ++i;
                continue;
            }
            if ((byte & 0xE0u) == 0xC0u) {
                needed = 1;
                if (byte < 0xC2u)
                    return false;
            } else if ((byte & 0xF0u) == 0xE0u) {
                needed = 2;
            } else if ((byte & 0xF8u) == 0xF0u) {
                needed = 3;
                if (byte > 0xF4u)
                    return false;
            } else {
                return false;
            }
            if (i + needed >= value.size())
                return false;
            for (std::size_t j = 1; j <= needed; ++j) {
                const auto continuation = static_cast<unsigned char>(value[i + j]);
                if ((continuation & 0xC0u) != 0x80u)
                    return false;
            }
            if (needed == 2) {
                const auto b1 = static_cast<unsigned char>(value[i + 1]);
                if (byte == 0xE0u && b1 < 0xA0u)
                    return false;
                if (byte == 0xEDu && b1 > 0x9Fu)
                    return false;
            } else if (needed == 3) {
                const auto b1 = static_cast<unsigned char>(value[i + 1]);
                if (byte == 0xF0u && b1 < 0x90u)
                    return false;
                if (byte == 0xF4u && b1 > 0x8Fu)
                    return false;
            }
            i += needed + 1;
        }
        return true;
    }
#ifdef _WIN32
    std::string format_windows_ansi_message(std::string_view value)
    {
        if (value.empty())
            return { };
        const int wide_count = ::MultiByteToWideChar(
            CP_ACP, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
        if (wide_count <= 0)
            return { };
        std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
        if (::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), wide.data(), wide_count)
            <= 0) {
            return { };
        }
        const int utf8_count = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_count, nullptr, 0, nullptr, nullptr);
        if (utf8_count <= 0)
            return { };
        std::string utf8(static_cast<std::size_t>(utf8_count), '\0');
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_count,
                utf8.data(), utf8_count, nullptr, nullptr)
            <= 0) {
            return { };
        }
        return utf8;
    }
#endif
} // namespace

namespace {
#ifdef _WIN32
    std::string format_windows_error_message(unsigned long code)
    {
        wchar_t* raw_buffer = nullptr;
        const DWORD chars = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&raw_buffer), 0, nullptr);

        if (chars == 0 || raw_buffer == nullptr) {
            return { };
        }

        std::wstring message(raw_buffer, chars);
        ::LocalFree(raw_buffer);

        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' || message.back() == L'\t')) {
            message.pop_back();
        }
        if (message.empty()) {
            return { };
        }

        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, message.data(), static_cast<int>(message.size()),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return { };
        }

        std::string utf8(static_cast<std::size_t>(required), '\0');
        const int converted = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, message.data(), static_cast<int>(message.size()),
            utf8.data(), required, nullptr, nullptr);
        if (converted <= 0) {
            return { };
        }
        utf8.resize(static_cast<std::size_t>(converted));
        return utf8;
    }
#endif
} // namespace

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

std::string format_system_error_message(const std::error_code& ec)
{
    if (!ec) {
        return { };
    }
#ifdef _WIN32
    if (const auto text = format_windows_error_message(static_cast<unsigned long>(ec.value()));
        !text.empty()) {
        return text;
    }
    // Some non-Win32 categories may not be renderable by FormatMessageW. In that
    // case prefer a stable code-only fallback rather than emitting a possibly
    // ACP-encoded narrow message.
    return std::string("system error ") + std::to_string(ec.value());
#else
    return ec.message();
#endif
}

std::string format_exception_message(const std::exception& e)
{
    if (const auto* system_error = dynamic_cast<const std::system_error*>(&e);
        system_error != nullptr) {
        return format_system_error_message(system_error->code());
    }

    const std::string_view raw(e.what() != nullptr ? e.what() : "");
    if (is_valid_utf8(raw)) {
        return std::string(raw);
    }
#ifdef _WIN32
    if (const auto converted = format_windows_ansi_message(raw); !converted.empty()) {
        return converted;
    }
#endif
    return std::string(raw);
}

void init_logger()
{
#ifdef _WIN32
    // aqua 日志文本统一以 UTF-8 生成。Windows console 若仍处于本地代码页，
    // 中文设备名会被错误解释成类似“鎵０鍣?”的乱码，因此在真正创建
    // stdout sink 前把 console 输出代码页切到 UTF-8。对重定向到文件的
    // stdout 不做额外处理，文件本身继续接收 UTF-8 字节流。
    if (HANDLE console = ::GetStdHandle(STD_OUTPUT_HANDLE);
        console != INVALID_HANDLE_VALUE && console != nullptr) {
        DWORD mode = 0;
        if (::GetConsoleMode(console, &mode)) {
            (void)::SetConsoleOutputCP(CP_UTF8);
        }
    }
#endif

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
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Fatal:
        return "fatal";
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
