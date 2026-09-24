#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H

// RAII / stream-lifecycle helpers shared by the WASAPI playback and capture
// backends: HANDLE ownership, MMCSS "Pro Audio" task registration, and the
// start-result coordination primitive. Previously each backend carried a
// near-verbatim copy; consolidated here.

// clang-format off: SDK 头顺序 load-bearing——avrt.h 不自带 windows.h，必须
// windows.h 先于 avrt.h，否则 HANDLE/BOOL/DECLSPEC_IMPORT 未定义；禁止排序。
#include <windows.h>
#include <avrt.h>
// clang-format on

#include "aqua/audio/audio_error.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>

namespace aqua::audio::wasapi {

// ---- HANDLE RAII ----
// 自动关闭由 CreateEventW / 其他 Win32 API 创建的 HANDLE。移动语义保留所有权，
// 析构时若持有合法句柄则 CloseHandle。
class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept
        : handle_(handle)
    {
    }

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (*this) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

    // 交出所有权但不关闭底层 HANDLE。
    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_ = nullptr;
};

// ---- MMCSS Pro Audio 任务注册 ----
// 在音频实时线程上注册 "Pro Audio" 任务，提升调度优先级；析构自动注销。
class ScopedMmcssTask final {
public:
    ScopedMmcssTask() noexcept
    {
        task_index_ = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index_);
        // 注册失败不打日志：本头文件被 client/server 两侧共用，分不清哪一侧；
        // 调用方在构造后用 registered() 自查，并在各自的 RT 宏下打带侧标签的行。
    }

    // MMCSS 注册是否成功。供调用方（知道自己是哪一侧）在各自 RT 宏下打日志。
    [[nodiscard]] bool registered() const noexcept { return handle_ != nullptr; }

    ~ScopedMmcssTask()
    {
        if (handle_ != nullptr) {
            ::AvRevertMmThreadCharacteristics(handle_);
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
    DWORD task_index_ = 0;
};

// ---- 启动状态协调 ----
// playback / capture 此前各自用一份完全相同的 StartState + signal_start_state
// 在音频线程与控制线程之间同步启动结果。统一到此共享类型与自由函数。
struct StreamStartState {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    AudioError result = AudioError::BackendFailed;
};

inline void signal_start_state(
    const std::shared_ptr<StreamStartState>& state, AudioError result) noexcept
{
    {
        std::lock_guard lock(state->mutex);
        state->result = result;
        state->completed = true;
    }
    state->cv.notify_one();
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H
