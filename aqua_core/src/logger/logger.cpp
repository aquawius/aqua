#include "aqua/logger/logger.h"

#include <memory>
#include <string>

// 文件 sink 与 dist_sink（--log-file 的 tee 用）两个平台都要，**必须放在下面的
// 平台分支之外**：放进 #else 会让 Android 编不过
// （error: no member named 'basic_file_sink_mt' in namespace 'spdlog::sinks'）。
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <spdlog/sinks/android_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <cstdlib>

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
    // 分派规则（两类值的语义不同，不能混）：
    //   1) generic_category = errno 语义（EAGAIN=11 在 Win32 表里是 ERROR_BAD_FORMAT，
    //      "语义完全错位"）→ 用 CRT strerror 的 errno 文本（MSVC 为 ASCII 英文，
    //      满足 UTF-8 契约）。std::thread 等标准库设施抛的 std::system_error 属此类。
    //   2) 其余类别（system_category，以及 asio 自己的 category——其 name 为
    //      "asio.system"、值就是 GetLastError/WSA 码）→ 统一 FormatMessageW + UTF-8。
    //      **不要**在这里改用 ec.message()：asio 的 message() 走 FormatMessageA，
    //      返回 ACP 窄字符，在 UTF-8 日志里是乱码（本文件顶部契约禁止）。
    //   3) 未知类别：没有可信渲染，退回 code-only，不猜编码。
    if (ec.category() == std::generic_category()) {
        return ec.message();
    }
    if (const auto text = format_windows_error_message(static_cast<unsigned long>(ec.value()));
        !text.empty()) {
        return text;
    }
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

void init_logger(std::string_view log_file)
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
    // 同步 sink（刻意不用 async）：每次调用返回前即写完，天然全序、零丢失，
    // 任何退出路径（return / 异常 / _Exit 前的显式排空）都不断行、不丢尾。
    // 同步写曾经的代价（控制台阻塞数 ms）已不成立：
    //   - 1s 诊断行在专用 diag 线程，不占网络 ioc；
    //   - 网络 strand / 音频 RT / dispatcher 热路径上没有无门控的逐包/逐回调日志
    //     （全是 trace 级或编译门控或纯异常路径），控制面日志都是低频事件。
    // 这里**刻意不调用 spdlog::shutdown()**：shutdown 会把 default logger 置空，
    // 之后任何静态析构里的日志即空指针解引用（log_* 均已判空保命，但消息会丢）。
    // 关机排空走显式的 shutdown_logger()（CLI 的 LogDrain 在 worker join 后调用）。
    std::shared_ptr<spdlog::sinks::sink> console;
#ifdef __ANDROID__
    console = std::make_shared<spdlog::sinks::android_sink_mt>("aqua");
#else
    console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
#endif
    // --log-file：tee（控制台 + 文件）。截断模式：一次运行一个文件，便于按 run 分析。
    // 刻意不做 per-sink 级别——文件与控制台同一条流，少一套要记的语义。
    std::shared_ptr<spdlog::sinks::sink> file;
    std::string file_error;
    if (!log_file.empty()) {
        try {
            file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                std::string(log_file), /*truncate=*/true);
        } catch (const spdlog::spdlog_ex& ex) {
            file_error = ex.what();
        }
    }
    std::shared_ptr<spdlog::logger> logger;
    if (file != nullptr) {
        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        dist->add_sink(console);
        dist->add_sink(file);
        logger = std::make_shared<spdlog::logger>("aqua", dist);
    } else {
        logger = std::make_shared<spdlog::logger>("aqua", console);
    }
    logger->set_level(spdlog::level::info);
    if (file != nullptr) {
        // 挂了文件 sink 就逐条 flush：文件 sink 走 libc 缓冲（4KB），进程被
        // SIGTERM/kill 时不会走 shutdown_logger()，低音量运行（server 启动只有
        // 十几行）会整段留在缓冲里丢掉（实测：tee 文件 0 字节）。逐条 flush 在
        // 274 行/s 的 --jb-trace 下也只是 274 次 fflush/s，可忽略。
        logger->flush_on(spdlog::level::trace);
    }
    spdlog::set_default_logger(std::move(logger));
    if (file == nullptr && !log_file.empty()) {
        // 到这里 default logger 已就绪，warn 能正常落地（日志失败不打断启动）。
        log_warn_fmt("--log-file open failed, console only: {} ({})", log_file, file_error);
    }
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

void shutdown_logger() noexcept
{
    try {
        if (const auto logger = spdlog::default_logger()) {
            logger->flush();
        }
    } catch (...) {
    }
}

void log_trace(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::trace, message);
    }
}
void log_debug(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::debug, message);
    }
}
void log_info(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::info, message);
    }
}
void log_warn(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::warn, message);
    }
}
void log_error(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::err, message);
    }
}
void log_fatal(std::string_view message)
{
    if (const auto logger = spdlog::default_logger_raw()) {
        logger->log(spdlog::level::critical, message);
    }
}

bool log_level_enabled(LogLevel level) noexcept
{
    const auto logger = spdlog::default_logger_raw();
    return logger != nullptr && logger->should_log(to_spdlog(level));
}

} // namespace aqua
