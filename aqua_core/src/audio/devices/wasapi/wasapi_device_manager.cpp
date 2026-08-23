#include "audio/devices/wasapi/wasapi_device_manager.h"

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

class ScopedComInitialization final {
public:
    ScopedComInitialization() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr) && hr != RPC_E_CHANGED_MODE;
        usable_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }

    ~ScopedComInitialization()
    {
        if (initialized_) {
            ::CoUninitialize();
        }
    }

    ScopedComInitialization(const ScopedComInitialization&) = delete;
    ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    bool initialized_ = false;
    bool usable_ = false;
};

struct ComReleaser {
    template <typename T>
    void operator()(T* value) const noexcept
    {
        if (value != nullptr) {
            value->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser>;

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

std::vector<AudioDevice>
WasapiAudioDeviceManager::enumerate(AudioDeviceDirection direction) const
{
    std::vector<AudioDevice> devices;
    const EDataFlow flow = to_data_flow(direction);
    if (flow != eCapture && flow != eRender) {
        return devices;
    }

    const ScopedComInitialization com;
    if (!com.usable()) {
        return devices;
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        return devices;
    }

    IMMDeviceCollection* raw_collection = nullptr;
    const HRESULT collection_hr = enumerator->EnumAudioEndpoints(
        flow,
        DEVICE_STATE_ACTIVE,
        &raw_collection);
    if (FAILED(collection_hr) || raw_collection == nullptr) {
        return devices;
    }
    ComPtr<IMMDeviceCollection> collection(raw_collection);

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return devices;
    }

    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* raw_device = nullptr;
        if (FAILED(collection->Item(index, &raw_device)) || raw_device == nullptr) {
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
        return std::nullopt;
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        return std::nullopt;
    }

    IMMDevice* raw_device = nullptr;
    const HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &raw_device);
    if (FAILED(hr) || raw_device == nullptr) {
        return std::nullopt;
    }
    ComPtr<IMMDevice> device(raw_device);

    return describe_device(*device, direction, true);
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
        return std::unexpected(AudioError::BackendFailed);
    }

    auto enumerator = create_enumerator();
    if (!enumerator) {
        return std::unexpected(AudioError::BackendFailed);
    }

    if (!requested) {
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
        return std::move(*described);
    }

    if (requested->empty()) {
        return std::unexpected(AudioError::InvalidArgument);
    }

    const std::wstring wide_id = wide_from_utf8(requested->value());
    if (wide_id.empty()) {
        return std::unexpected(AudioError::InvalidArgument);
    }
    IMMDevice* raw_device = nullptr;
    const HRESULT get_hr = enumerator->GetDevice(wide_id.c_str(), &raw_device);
    if (FAILED(get_hr) || raw_device == nullptr) {
        return std::unexpected(AudioError::DeviceNotFound);
    }
    ComPtr<IMMDevice> device(raw_device);

    auto described = describe_device_with_query(*device, false);
    if (!described) {
        return std::unexpected(AudioError::DeviceNotFound);
    }

    if (described->direction != direction) {
        return std::unexpected(AudioError::DeviceNotFound);
    }

    return std::move(*described);
}

} // namespace aqua::audio::wasapi
