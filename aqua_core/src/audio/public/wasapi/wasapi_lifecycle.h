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

#include <chrono>
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

// 启动握手的等待上限。
//
// 为什么需要：握手期间音频线程正在做CoCreateInstance / GetMixFormat / Activate
// 这一串 COM 调用，**控制线程完全阻塞在cv.wait 上，既不能超时也不会响应
// stop()**（stop() 由同一个控制线程调用，没人调得到）。正常路径音频线程必会
// signal_start_state（每个早退分支 + 两个 catch 都覆盖了），但坏驱动 / 被拔掉的
// 设备上COM 调用长时间不返回时，start() 就永久挂死，表现为客户端"卡在连接中"
// 且 stop() 也救不回来。
//
// 数值依据：健康路径是纯内存操作（毫秒级）；冷启动、驱动尚未加载的共享引擎，
// openStream 通常在几十~几百毫秒。取值往"宁可慢"一侧偏，因为这个超时替代的是
// **无限等待**，它要挡的是挂死而不是"启动偏慢的合法设备"——
//
//	在 Windows 上 Initialize 一个蓝牙 / 虚拟声卡端点走的是 profile 协商，
//	实测可以到数秒；2s 那样的值会把"慢但在正常工作"的设备判成 BackendFailed，
//	那是这个超时**引入**的新故障模式，比原来的挂死更难排查（挂死至少有症状，
//	误判表现为"偶发连不上"）。5s 仍然有界（日志会指出卡在哪一步），同时与
//	SESSION_TIMEOUT 同量级，符合本项目对"多久算异常"的既有刻度。
//
//	想更快发现坏设备，应该收紧的是 `resolve()` 的设备探测，而不是这个握手等待：
//	探测失败会立刻返回，而探测成功之后卡住才说明是端点打开阶段的问题。
inline constexpr std::chrono::milliseconds kStartHandshakeTimeout { 5000 };

// 有界等待握手结果。返回 true = 音频线程已 signal；false = 超时。
//
// 超时后的正确处置由调用方决定（统一是 stop() + BackendFailed 回滚）。
// 注意 stop() 里的 join 仍会等音频线程真正退出——所以这个超时能把"无声挂死"
// 变成"有日志的失败 + 尽力收敛"，但**不能**把一个永久阻塞的 COM 调用变成
// 可恢复错误：那属于驱动/OS 层面的问题，超时只是让它不再静默。
inline bool wait_for_start_state(const std::shared_ptr<StreamStartState>& state) noexcept
{
    std::unique_lock lock(state->mutex);
    return state->cv.wait_for(lock, kStartHandshakeTimeout, [&] { return state->completed; });
}

} // namespace aqua::audio::wasapi

#endif // AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H
