#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_STRING_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_STRING_H

// UTF-8 <-> wide string conversion shared by the WASAPI playback, capture and
// device-manager backends. Previously each carried its own copy (the device
// manager had a standalone implementation; playback / capture inlined the same
// MultiByteToWideChar logic). Consolidated here to keep it single-sourced.

#include <windows.h>

#include <string>

namespace aqua::audio::wasapi {

[[nodiscard]] inline std::wstring wide_from_utf8(const std::string& value) noexcept
{
    if (value.empty()) {
        return { };
    }

    const int length = static_cast<int>(value.size());
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) {
        return { };
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int converted = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), required);
    if (converted <= 0) {
        return { };
    }

    result.resize(static_cast<std::size_t>(converted));
    return result;
}

[[nodiscard]] inline std::string utf8_from_wide(const wchar_t* value) noexcept
{
    if (value == nullptr || *value == L'\0') {
        return { };
    }

    const int length = static_cast<int>(::wcslen(value));
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return { };
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, length, result.data(), required, nullptr, nullptr);
    if (converted <= 0) {
        return { };
    }

    result.resize(static_cast<std::size_t>(converted));
    return result;
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_STRING_H
