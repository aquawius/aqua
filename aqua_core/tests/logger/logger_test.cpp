#include <gtest/gtest.h>

#include <stdexcept>

#include "aqua/logger/logger.h"

#ifdef _WIN32
#include <cwchar>
#include <winsock2.h>
#endif

TEST(LogTest, DefaultLevelAndParsing)
{
    EXPECT_EQ(aqua::default_log_level(), aqua::LogLevel::Info);

    const auto fatal = aqua::string_to_log_level_enum("fatal");
    ASSERT_TRUE(fatal.has_value());
    EXPECT_EQ(*fatal, aqua::LogLevel::Fatal);
    EXPECT_EQ(*aqua::string_to_log_level_enum("critical"), aqua::LogLevel::Fatal);
    EXPECT_EQ(*aqua::string_to_log_level_enum("warning"), aqua::LogLevel::Warn);
    EXPECT_EQ(*aqua::string_to_log_level_enum("warn"), aqua::LogLevel::Warn);
    EXPECT_FALSE(aqua::string_to_log_level_enum("verbose").has_value());
}

TEST(LogTest, LogLevelNames)
{
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Trace), "trace");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Debug), "debug");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Info), "info");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Warn), "warn");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Error), "error");
    EXPECT_STREQ(aqua::log_level_name(aqua::LogLevel::Fatal), "fatal");
}

TEST(LogTest, SetLevelAndLog)
{
    aqua::set_log_level(aqua::LogLevel::Warn);

    EXPECT_FALSE(aqua::log_level_enabled(aqua::LogLevel::Debug));
    EXPECT_TRUE(aqua::log_level_enabled(aqua::LogLevel::Warn));
    EXPECT_TRUE(aqua::log_level_enabled(aqua::LogLevel::Fatal));

    aqua::log_trace("trace message");
    aqua::log_debug("debug message");
    aqua::log_info("info message");
    aqua::log_warn("warn message");
    aqua::log_error("error message");
    aqua::log_fatal("fatal message");

    aqua::log_info_fmt("formatted {}", "info");
    aqua::log_error_fmt("formatted {}", "error");

    aqua::set_log_level(aqua::LogLevel::Info);
}

namespace {
bool is_valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c <= 0x7F) {
            extra = 0;
        } else if ((c & 0xE0u) == 0xC0u) {
            extra = 1;
            if (c < 0xC2u)
                return false;
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2;
            if (c == 0xE0u && i + 1 < text.size()
                && static_cast<unsigned char>(text[i + 1]) < 0xA0u)
                return false;
            if (c == 0xEDu && i + 1 < text.size()
                && static_cast<unsigned char>(text[i + 1]) >= 0xA0u)
                return false;
        } else if ((c & 0xF8u) == 0xF0u) {
            extra = 3;
            if (c > 0xF4u)
                return false;
        } else {
            return false;
        }
        if (i + extra >= text.size())
            return false;
        for (std::size_t j = 1; j <= extra; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0u) != 0x80u)
                return false;
        }
        i += extra + 1;
    }
    return true;
}
} // namespace

TEST(LogTest, SystemErrorMessageIsUtf8)
{
#ifdef _WIN32
    const std::error_code ec(WSAEADDRINUSE, std::system_category());
#else
    const std::error_code ec = std::make_error_code(std::errc::address_in_use);
#endif
    const auto message = aqua::format_system_error_message(ec);
    EXPECT_FALSE(message.empty());
    EXPECT_TRUE(is_valid_utf8(message));
}

TEST(LogTest, EmptySystemErrorMessageForSuccessCode)
{
    EXPECT_TRUE(aqua::format_system_error_message({ }).empty());
}

TEST(LogTest, ExceptionMessageUsesUtf8SafeSystemErrorFormatting)
{
#ifdef _WIN32
    const std::system_error error(WSAEADDRINUSE, std::system_category(), "ignored narrow message");
#else
    const std::system_error error(std::make_error_code(std::errc::address_in_use), "ignored narrow message");
#endif
    const auto message = aqua::format_exception_message(error);
    EXPECT_FALSE(message.empty());
    EXPECT_TRUE(is_valid_utf8(message));
}

TEST(LogTest, ExceptionMessagePreservesUtf8Text)
{
    const std::runtime_error error("UTF-8: \xE4\xB8\xAD\xE6\x96\x87");
    EXPECT_EQ(aqua::format_exception_message(error), "UTF-8: \xE4\xB8\xAD\xE6\x96\x87");
}

#ifdef _WIN32
TEST(LogTest, ExceptionMessageConvertsWindowsAcpWhenNeeded)
{
    constexpr wchar_t kText[] = L"提供了一个无效的参数";
    const int required = ::WideCharToMultiByte(CP_ACP, 0, kText, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        GTEST_SKIP() << "current Windows ANSI code page cannot represent the test text";
    }
    std::string narrow(static_cast<std::size_t>(required - 1), '\0');
    // 用显式长度转换（不含结尾空字符），否则 cchWideChar=-1 需要 required 字节，
    // 而这里缓冲只有 required-1 字节，会返回 0。
    ASSERT_GT(::WideCharToMultiByte(CP_ACP, 0, kText,
                  static_cast<int>(::wcslen(kText)), narrow.data(), required - 1, nullptr, nullptr),
        0);
    const std::runtime_error error(narrow);
    const auto message = aqua::format_exception_message(error);
    EXPECT_TRUE(is_valid_utf8(message));
}
#endif
