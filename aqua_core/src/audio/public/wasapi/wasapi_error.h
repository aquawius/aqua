#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ERROR_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ERROR_H

// HRESULT -> AudioError mapping and HRESULT string formatting shared by the
// WASAPI playback and capture backends. Both previously maintained a verbatim
// copy of these mappings; consolidated here.

// clang-format off: windows.h 必须先于 audioclient.h——本文件直接 switch
// AUDCLNT_E_*（定义于 audioclient.h），且 audioclient.h 依赖 windows.h 基础
// 类型；顺序 load-bearing，禁止排序。
#include <windows.h>
#include <audioclient.h>
// clang-format on

#include "aqua/audio/audio_error.h"

#include <cstdio>
#include <string>

namespace aqua::audio::wasapi {

[[nodiscard]] inline std::string hresult_hex(HRESULT hr)
{
    // "0x" + 8 位十六进制 + NUL 只需 11 字节；32 留足余量避免格式误用。
    constexpr std::size_t kHresultHexBufferBytes = 32;
    char buffer[kHresultHexBufferBytes] { };
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
    return buffer;
}

// 流建立阶段（start）的 HRESULT -> AudioError 映射。playback 与 capture 此前
// 各自维护一份完全相同的映射。
[[nodiscard]] inline AudioError map_start_hresult(HRESULT hr) noexcept
{
    switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return AudioError::FormatUnsupported;
    case AUDCLNT_E_DEVICE_IN_USE:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return AudioError::DeviceUnavailable;
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return AudioError::DeviceDisconnected;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return AudioError::BackendFailed;
    case E_ACCESSDENIED:
        return AudioError::PermissionDenied;
    case E_INVALIDARG:
        return AudioError::InvalidArgument;
    default:
        return AudioError::BackendFailed;
    }
}

// 流运行期（GetBuffer / ReleaseBuffer 等）的 HRESULT -> AudioError 映射。
// 运行期已不可能出现"格式不支持 / 设备被占用"等建立期错误，仅映射失效类。
[[nodiscard]] inline AudioError map_runtime_hresult(HRESULT hr) noexcept
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        return AudioError::DeviceDisconnected;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return AudioError::BackendFailed;
    default:
        return AudioError::BackendFailed;
    }
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_ERROR_H
