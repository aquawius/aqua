#ifndef AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H
#define AQUA_AUDIO_PUBLIC_WASAPI_WASAPI_LIFECYCLE_H

// RAII / stream-lifecycle helpers shared by the WASAPI playback and capture
// backends: HANDLE ownership, MMCSS task registration (two-tier ladder), and the
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

// ---- MMCSS 任务注册（两级降级链）----
// 在音频实时线程上注册 MMCSS 任务以提升调度优先级；析构自动注销。
//
// 为什么要降级链：MMCSS 只是"更好的调度优先级"，**拿不到也应当照常工作**，所以注册
// 失败不改变启动结果。而任务档在各 Windows 版本 / 被裁剪的镜像里可能不全（任务名不
// 存在时 AvSetMmThreadCharacteristicsW 直接返回 NULL），换一个档名就能过 —— 这就是
// fallback 的全部意义；两档都失败就不使用 MMCSS。
//
// 调用方契约：构造后**必须**按 outcome() 打一条**无条件**日志（旧实现把失败日志藏在
// RT 调试宏里，release 下完全静默，"这条链路没有实时优先级"因此永远无从知晓）。
class ScopedMmcssTask final {
public:
    enum class Outcome {
        Preferred, // 首选档注册成功
        Fallback,  // 首选档失败，降级档成功
        None,      // 两档都失败：本次不使用 MMCSS（调用方继续运行）
    };

    // 注册动作可注入：默认转发到系统 API；单测传假注册器即可覆盖三条路径（本机
    // 无法自然构造 MMCSS 注册失败）。用 decltype 照抄 SDK 声明的类型（含调用约定），
    // 避免手写签名与之不符。
    struct Api {
        using AcquireFn = decltype(&::AvSetMmThreadCharacteristicsW);
        using ReleaseFn = decltype(&::AvRevertMmThreadCharacteristics);

        AcquireFn acquire = &::AvSetMmThreadCharacteristicsW;
        ReleaseFn release = &::AvRevertMmThreadCharacteristics;
    };

    // preferred 失败则试 fallback（nullptr = 没有降级档）；两档都失败 = 未注册。
    explicit ScopedMmcssTask(const wchar_t* preferred, const wchar_t* fallback = nullptr,
        Api api = { }) noexcept
        : api_(api)
    {
        if (preferred != nullptr && try_acquire(preferred)) {
            outcome_ = Outcome::Preferred;
            return;
        }
        if (fallback != nullptr && try_acquire(fallback)) {
            outcome_ = Outcome::Fallback;
            return;
        }
        outcome_ = Outcome::None;
    }

    ~ScopedMmcssTask()
    {
        if (handle_ != nullptr) {
            (void)api_.release(handle_);
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

    [[nodiscard]] Outcome outcome() const noexcept { return outcome_; }
    // MMCSS 注册是否成功（成功 = 本线程拿到了实时调度优先级）。
    [[nodiscard]] bool registered() const noexcept { return handle_ != nullptr; }
    // 生效的档名；未注册时为 nullptr。
    [[nodiscard]] const wchar_t* task_name() const noexcept { return task_name_; }
    // 最近一次注册失败的错误码（全程成功为 0）。常见值：ERROR_NOT_FOUND（该任务档
    // 在本机不存在）、ERROR_ACCESS_DENIED（策略禁用了 MMCSS）。
    [[nodiscard]] unsigned long last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] bool try_acquire(const wchar_t* name) noexcept
    {
        DWORD task_index = 0;
        HANDLE handle = api_.acquire(name, &task_index);
        if (handle == nullptr) {
            last_error_ = ::GetLastError();
            return false;
        }
        handle_ = handle;
        task_name_ = name;
        return true;
    }

    HANDLE handle_ = nullptr;
    const wchar_t* task_name_ = nullptr;
    Outcome outcome_ = Outcome::None;
    unsigned long last_error_ = 0;
    Api api_ { };
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
