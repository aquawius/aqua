#ifndef AQUA_AUDIO_WASAPI_COM_H
#define AQUA_AUDIO_WASAPI_COM_H

#include <windows.h>
#include <objbase.h>

#include <memory>

namespace aqua::audio::wasapi {

// COM 接口持有者：通过 IUnknown::Release 释放。与 std::unique_ptr 搭配，
// 避免每个后端重复手写 AddRef/Release。
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

// COM apartment 初始化是每线程的。每个直接使用 COM/WASAPI 的线程为自己初始化 COM；
// 本辅助类不强加全局进程级的 apartment 模型。
class ScopedComInitialization final {
public:
    ScopedComInitialization() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
        // RPC_E_CHANGED_MODE 表示调用线程已被初始化到另一个 apartment。
        // 已存在的 COM 初始化仍可用；只是不能再对其调用 CoUninitialize()。
        usable_ = initialized_ || hr == RPC_E_CHANGED_MODE;
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

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_WASAPI_COM_H
