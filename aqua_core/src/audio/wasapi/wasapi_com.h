#ifndef AQUA_AUDIO_WASAPI_COM_H
#define AQUA_AUDIO_WASAPI_COM_H

#include <windows.h>
#include <objbase.h>

#include <memory>

namespace aqua::audio::wasapi {

// COM interface holder that releases via IUnknown::Release. Pairs with
// std::unique_ptr to avoid duplicating manual AddRef/Release in each backend.
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

// COM apartment initialization is per-thread. Each thread that directly uses
// COM/WASAPI initializes COM for itself; this helper does not impose a global
// process-wide apartment model.
class ScopedComInitialization final {
public:
    ScopedComInitialization() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
        // RPC_E_CHANGED_MODE means the calling thread was already initialized
        // into another apartment. Existing COM initialization is still usable;
        // we simply must not call CoUninitialize() for it.
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
