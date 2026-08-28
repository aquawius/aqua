#include "audio/devices/wasapi/wasapi_device_manager.h"
#include "audio/wasapi/wasapi_com.h"
#include "aqua/logger/logger.h"

// 注意包含顺序：functiondiscoverykeys_devpkey.h 在本 SDK（10.0.26100）中不自带
// DEFINE_PROPERTYKEY（其内部 #include <devpropdef.h> 被注释掉），该宏由
// mmdeviceapi.h（经 propsys.h -> propkeydef.h）提供并重新定义，因此必须先包含
// mmdeviceapi.h 再包含 functiondiscoverykeys_devpkey.h。
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <propidl.h>
#include <propsys.h>
#include <windows.h>

#include <cwchar>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace aqua::audio::wasapi {
namespace {

[[nodiscard]] ComPtr<IMMDeviceEnumerator> create_enumerator() noexcept
{
    IMMDeviceEnumerator* raw = nullptr;
    const HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&raw));

    if (FAILED(hr)) {
        return {};
    }
    return ComPtr<IMMDeviceEnumerator>(raw);
}

[[nodiscard]] EDataFlow to_data_flow(AudioDeviceDirection direction) noexcept
{
    switch (direction) {
    case AudioDeviceDirection::INPUT:
        return eCapture;
    case AudioDeviceDirection::OUTPUT:
        return eRender;
    case AudioDeviceDirection::NONE:
        break;
    }
    return EDataFlow(-1);
}

[[nodiscard]] AudioDeviceDirection from_data_flow(EDataFlow flow) noexcept
{
    switch (flow) {
    case eCapture:
        return AudioDeviceDirection::INPUT;
    case eRender:
        return AudioDeviceDirection::OUTPUT;
    case eAll:
        break;
    }
    return AudioDeviceDirection::NONE;
}

[[nodiscard]] std::string utf8_from_wide(const wchar_t* value) noexcept
{
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int length = static_cast<int>(::wcslen(value));
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value,
        length,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int converted = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value,
        length,
        result.data(),
        required,
        nullptr,
        nullptr);
    if (converted <= 0) {
        return {};
    }

    result.resize(static_cast<std::size_t>(converted));
    return result;
}

[[nodiscard]] std::wstring wide_from_utf8(const std::string& value) noexcept
{
    if (value.empty()) {
        return {};
    }

    const int length = static_cast<int>(value.size());
    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        length,
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int converted = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        length,
        result.data(),
        required);
    if (converted <= 0) {
        return {};
    }

    result.resize(static_cast<std::size_t>(converted));
    return result;
}

[[nodiscard]] std::string device_id(IMMDevice& device) noexcept
{
    LPWSTR raw_id = nullptr;
    const HRESULT hr = device.GetId(&raw_id);
    if (FAILED(hr) || raw_id == nullptr) {
        return {};
    }

    const std::string result = utf8_from_wide(raw_id);
    ::CoTaskMemFree(raw_id);
    return result;
}

[[nodiscard]] std::string default_endpoint_id(
    IMMDeviceEnumerator& enumerator,
    EDataFlow flow) noexcept
{
    IMMDevice* raw_device = nullptr;
    const HRESULT hr = enumerator.GetDefaultAudioEndpoint(flow, eConsole, &raw_device);
    if (FAILED(hr) || raw_device == nullptr) {
        return {};
    }
    ComPtr<IMMDevice> device(raw_device);
    return device_id(*device);
}

[[nodiscard]] std::string friendly_name(IMMDevice& device) noexcept
{
    IPropertyStore* raw_store = nullptr;
    const HRESULT open_hr = device.OpenPropertyStore(STGM_READ, &raw_store);
    if (FAILED(open_hr) || raw_store == nullptr) {
        return {};
    }
    ComPtr<IPropertyStore> store(raw_store);

    PROPVARIANT value;
    ::PropVariantInit(&value);
    const HRESULT get_hr = store->GetValue(PKEY_Device_FriendlyName, &value);

    std::string result;
    if (SUCCEEDED(get_hr)) {
        if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            result = utf8_from_wide(value.pwszVal);
        } else if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
            result = utf8_from_wide(value.bstrVal);
        }
    }

    ::PropVariantClear(&value);
    return result;
}

[[nodiscard]] bool is_active(IMMDevice& device) noexcept
{
    DWORD state = DEVICE_STATE_DISABLED;
    return SUCCEEDED(device.GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

[[nodiscard]] std::optional<AudioDevice> describe_device(
    IMMDevice& device,
    AudioDeviceDirection expected_direction,
    bool is_default) noexcept
{
    if (!is_active(device)) {
        return std::nullopt;
    }

    std::string id = device_id(device);
    if (id.empty()) {
        return std::nullopt;
    }

    std::string name = friendly_name(device);
    if (name.empty()) {
        name = id;
    }


    AudioDevice result;
    result.id = AudioDeviceId(std::move(id));
    result.name = std::move(name);
    result.direction = expected_direction;
    result.is_default = is_default;
    return result;
}

[[nodiscard]] std::optional<AudioDevice> describe_device_with_query(
    IMMDevice& device,
    bool is_default) noexcept
{
    IMMEndpoint* endpoint = nullptr;
    const HRESULT hr = device.QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&endpoint));
    if (FAILED(hr) || endpoint == nullptr) {
        return std::nullopt;
    }
    ComPtr<IMMEndpoint> endpoint_ptr(endpoint);

    EDataFlow flow = eAll;
    if (FAILED(endpoint_ptr->GetDataFlow(&flow))) {
        return std::nullopt;
    }

    const AudioDeviceDirection direction = from_data_flow(flow);
    if (direction == AudioDeviceDirection::NONE) {
        return std::nullopt;
    }

    return describe_device(device, direction, is_default);
}

[[nodiscard]] AudioError error_from_hresult(HRESULT hr) noexcept
{
    if (hr == E_INVALIDARG) {
        return AudioError::InvalidArgument;
    }

    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        return AudioError::PermissionDenied;
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return AudioError::DeviceNotFound;
    }

    return AudioError::BackendFailed;
}

} // namespace

WasapiAudioDeviceManager::WasapiAudioDeviceManager()
{
    log_debug("WASAPI AudioDeviceManager backend instance created");
}

std::vector<AudioDevice>
WasapiAudioDeviceManager::enumerate(AudioDeviceDirection direction) const
{
    std::vector<AudioDevice> devices;
    const EDataFlow flow = to_data_flow(direction);
    if (flow != eCapture && flow != eRender) {
        log_debug_fmt("WASAPI enumerate ignored invalid direction={}", static_cast<int>(direction));
        return devices;
    }

    const ScopedComInitialization com;
    if (!com.usable()) {
        log_debug("WASAPI enumerate: COM initialization unavailable");
        return devices;
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        log_debug("WASAPI enumerate: failed to create MMDeviceEnumerator");
        return devices;
    }

    IMMDeviceCollection* raw_collection = nullptr;
    const HRESULT collection_hr = enumerator->EnumAudioEndpoints(
        flow,
        DEVICE_STATE_ACTIVE,
        &raw_collection);
    if (FAILED(collection_hr) || raw_collection == nullptr) {
        log_debug_fmt("WASAPI enumerate: EnumAudioEndpoints failed hr=0x{:08X}", static_cast<unsigned>(collection_hr));
        return devices;
    }
    ComPtr<IMMDeviceCollection> collection(raw_collection);

    UINT count = 0;
    const HRESULT count_hr = collection->GetCount(&count);
    if (FAILED(count_hr)) {
        log_debug_fmt("WASAPI enumerate: IMMDeviceCollection::GetCount failed hr=0x{:08X}", static_cast<unsigned>(count_hr));
        return devices;
    }

    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* raw_device = nullptr;
        const HRESULT item_hr = collection->Item(index, &raw_device);
        if (FAILED(item_hr) || raw_device == nullptr) {
            log_trace_fmt("WASAPI enumerate: device index {} unavailable hr=0x{:08X}",
                index, static_cast<unsigned>(item_hr));
            continue;
        }
        ComPtr<IMMDevice> device(raw_device);

        auto described = describe_device(*device, direction, false);
        if (described) {
            devices.push_back(std::move(*described));
        }
    }

    auto default_device_result = default_device(direction);
    if (default_device_result) {
        for (auto& device : devices) {
            if (device.id == default_device_result->id) {
                device.is_default = true;
                break;
            }
        }
    }

    for (const auto& device : devices) {
        log_debug_fmt("WASAPI device: direction={} id='{}' name='{}' default={}",
            static_cast<int>(device.direction), device.id.value(), device.name, device.is_default);
    }
    log_debug_fmt("WASAPI device enumeration complete: direction={} count={}",
        static_cast<int>(direction), devices.size());
    return devices;
}

std::optional<AudioDevice>
WasapiAudioDeviceManager::default_device(AudioDeviceDirection direction) const
{
    const EDataFlow flow = to_data_flow(direction);
    if (flow != eCapture && flow != eRender) {
        return std::nullopt;
    }

    const ScopedComInitialization com;
    if (!com.usable()) {
        log_debug("WASAPI default_device: COM initialization unavailable");
        return std::nullopt;
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        log_debug("WASAPI default_device: failed to create MMDeviceEnumerator");
        return std::nullopt;
    }

    IMMDevice* raw_device = nullptr;
    const HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &raw_device);
    if (FAILED(hr) || raw_device == nullptr) {
        log_debug_fmt("WASAPI default device lookup failed: direction={} hr=0x{:08X}", static_cast<int>(direction), static_cast<unsigned>(hr));
        return std::nullopt;
    }
    ComPtr<IMMDevice> device(raw_device);

    auto described = describe_device(*device, direction, true);
    if (described) {
        log_debug_fmt("WASAPI default device: direction={} id='{}' name='{}'",
            static_cast<int>(direction), described->id.value(), described->name);
    }
    return described;
}

std::expected<AudioDevice, AudioError>
WasapiAudioDeviceManager::resolve(
    AudioDeviceDirection direction,
    const std::optional<AudioDeviceId>& requested) const
{
    const EDataFlow flow = to_data_flow(direction);
    if (flow != eCapture && flow != eRender) {
        return std::unexpected(AudioError::InvalidArgument);
    }

    const ScopedComInitialization com;
    if (!com.usable()) {
        log_debug("WASAPI resolve: COM initialization unavailable");
        return std::unexpected(AudioError::BackendFailed);
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        log_debug("WASAPI resolve: failed to create MMDeviceEnumerator");
        return std::unexpected(AudioError::BackendFailed);
    }

    if (!requested) {
        log_debug_fmt("WASAPI resolving default device: direction={}", static_cast<int>(direction));
        IMMDevice* raw_device = nullptr;
        const HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &raw_device);
        if (FAILED(hr) || raw_device == nullptr) {
            return std::unexpected(error_from_hresult(hr));
        }
        ComPtr<IMMDevice> device(raw_device);

        auto described = describe_device(*device, direction, true);
        if (!described) {
            return std::unexpected(AudioError::DeviceNotFound);
        }
        log_debug_fmt("WASAPI resolved default device: direction={} id='{}' name='{}'",
            static_cast<int>(direction), described->id.value(), described->name);
        return std::move(*described);
    }

    if (requested->empty()) {
        log_error("WASAPI resolve: requested device id is empty");
        return std::unexpected(AudioError::InvalidArgument);
    }

    const std::wstring wide_id = wide_from_utf8(requested->value());
    if (wide_id.empty()) {
        log_error("WASAPI resolve: requested device id could not be converted from UTF-8");
        return std::unexpected(AudioError::InvalidArgument);
    }
    IMMDevice* raw_device = nullptr;
    const HRESULT get_hr = enumerator->GetDevice(wide_id.c_str(), &raw_device);
    if (FAILED(get_hr) || raw_device == nullptr) {
        log_debug_fmt("WASAPI resolve: requested device not found hr=0x{:08X}", static_cast<unsigned>(get_hr));
        // 非空但无法解析的 id 视为「设备未找到」，
        // 即使 GetDevice 会以 E_INVALIDARG 拒绝格式非法的字符串。
        return std::unexpected(AudioError::DeviceNotFound);
    }
    ComPtr<IMMDevice> device(raw_device);

    auto described = describe_device_with_query(*device, false);
    if (!described) {
        return std::unexpected(AudioError::DeviceNotFound);
    }

    if (described->direction != direction) {
        log_debug_fmt("WASAPI resolve: device direction mismatch requested={} actual={}", static_cast<int>(direction), static_cast<int>(described->direction));
        return std::unexpected(AudioError::DeviceNotFound);
    }

    described->is_default = described->id.value() == default_endpoint_id(*enumerator, flow);
    log_debug_fmt("WASAPI resolved device: direction={} id='{}' name='{}' default={}",
        static_cast<int>(direction), described->id.value(), described->name, described->is_default);
    return std::move(*described);
}

} // namespace aqua::audio::wasapi
