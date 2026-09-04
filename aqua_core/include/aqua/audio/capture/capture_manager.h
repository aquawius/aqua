#ifndef AQUA_AUDIO_CAPTURE_CAPTURE_MANAGER_H
#define AQUA_AUDIO_CAPTURE_CAPTURE_MANAGER_H

// CaptureManager：采集生命周期的管理边界（doc/capture_switching_design.md）。
//
// 层级：ServerRuntime --> CaptureManager --> AudioCapture --> WASAPI
//
// 职责边界（对称 client PlaybackManager，防止切换策略下沉到 backend）：
//   - CaptureManager：active device、switching 状态、stop/start 顺序、
//     候选链回滚、重试预算、诊断；
//   - AudioCapture：打开设备、创建流、callback 生命周期、backend error 转换。
// AudioCapture 不提供 switch_device 之类的策略 API；restart 事务全部在
// CaptureManager 内编排为 stop -> start 序列。
//
// restart 事务链（capture_switching_design.md §5）：
//   Switching -> 捕获 previous_active_device -> stop 旧流（同步 join 音频
//   线程，AudioCapture::stop 契约保证返回后旧回调不再访问 packetizer）
//   -> 依次尝试去重候选 [target, previous, system_default]
//   -> 首个成功者 Running；链耗尽 -> Fatal（终态，决策者可据此终止会话）。
//   全程不触碰 packetizer / network / session——seq 与时间线归它们所有，
//   capture 生命周期 ≠ 流时间线（§8 时间线不变式）。
//
// 格式不可变（共享原则）：首流成功后会话格式钉进 active_config（显式
// format），后续候选 start 必须原生支持该格式（encoding + channels 严格
// 相等；WASAPI 由 IsFormatSupported/Initialize 拒绝），否则该候选按
// FormatUnsupported 处理并尝试下一候选。
//
// 路由模型（§4，枚举定义见 capture_route_mode.h；复用
// (source, optional<device>)，无 PreferCurrent）：
//   - config.device == nullopt -> FollowSystem（跟随该 source 方向的系统默认）；
//   - config.device 有值      -> PreferredDevice（sticky 用户意图，钉住该
//     设备；不可用即 Fatal -> stop，不降级到系统默认）。运行期无手动切换
//     入口，sticky = CLI 配置值。
//   source 方向运行期不可改。
//
// 防抖（§5）：所有自动 restart（错误驱动 + 默认变化驱动）共享同一个
// 10s 窗口最多 3 次的预算，超限按链耗尽处理（Fatal）。server 无手动切换，
// 无窗口重置来源。
//
// 线程约定：start/restart_on_error/tick/stop 必须由同一控制线程串行调用
// （ServerRuntime 生命周期路径，经 lifecycle_mutex_ 串行化）；查询
// （state/route/active_device/last_switch_result/stats）任意线程。
// block/event 回调运行在 backend 线程，禁止在回调内调用本类任何方法。

#include "aqua/audio/audio_switch_result.h"
#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/capture/capture_route_mode.h"
#include "aqua/audio/capture/capture_state.h"
#include "aqua/audio/devices/audio_device.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace aqua::audio {

// CaptureSwitchState / CaptureRouteMode 已移至独立头文件
// （capture_state.h / capture_route_mode.h），与 playback 侧
// （playback_state.h / playback_route_mode.h）对称拆分。

class AudioDeviceManager;

class CaptureManager final {
public:
    // 创建平台采集后端；平台不支持时 available() == false。
    explicit CaptureManager(AudioDeviceManager& device_manager);

    // 直接注入后端实例（测试用）。device_manager 可为 nullptr：
    // 此时跳过候选解析（active_device 直接取请求值）且 tick() 不动作。
    explicit CaptureManager(std::unique_ptr<AudioCapture> capture,
        AudioDeviceManager* device_manager = nullptr);

    CaptureManager(const CaptureManager&) = delete;
    CaptureManager& operator=(const CaptureManager&) = delete;

    // 启动采集：转发 AudioCapture::start，并维护 CaptureSwitchState
    // （Starting -> Running；失败回 Inactive）。
    // 成功后记住 config 与回调（restart 复用）；回调经 shared bundle 保活
    // （AudioCaptureCallback 不可拷贝，restart 需要重新传入同一回调）。
    // 会话格式在首流成功后钉进 active_config（info() 回读，Format
    // immutable 的载体）。路由模式由 config.device 推导。
    std::expected<void, AudioError>
    start(const AudioCaptureConfig& config,
        AudioCaptureCallback block_callback,
        AudioCaptureEventCallback event_callback = { }) noexcept;

    // 错误驱动的自动 restart（设备拔出/失效，§6 路径 1）：
    // 按路由模式推导目标（FollowSystem -> nullopt；PreferredDevice ->
    // sticky 配置设备），走候选链。受共享重试预算约束（10s/3，超限 Fatal）。
    // 不改变路由模式：FollowSystem 的 fallback 是临时降级，用户意图不动；
    // PreferredDevice 失败即 Fatal（钉住意图不被降级覆盖）。
    std::expected<SwitchResult, AudioError> restart_on_error() noexcept;

    // 路由状态轮询（§6 路径 2，由 ServerRuntime 的 control tick 每 500ms
    // 调用，已在生命周期串行路径内）：仅 FollowSystem 模式查询该 source
    // 方向的系统默认设备，若与当前实际设备不同则 restart 跟随（与错误
    // 驱动共享重试预算）。设备查询与切换决策收敛在本类，不污染 backend
    // 与 runtime。PreferredDevice 不查询也不跟随（用户意图优先）。
    //
    // 返回值：本次调用是否执行了跟随切换事务（switch_to）。true = 已
    // 切换（结果看 state()/last_switch_result()，可能成功也可能 Fatal）；
    // false = 无事发生（默认未变 / 非 FollowSystem / 非 Running / 预算
    // 超限直接 Fatal 未触事务）。决策者用它区分"事务处理了本次设备变化"
    // 与"纯轮询"，以吸收切换前后 latch 的滞留设备错误标志（对称 client
    // 侧 PlaybackManager::on_devices_changed 的 bool 返回契约）。
    [[nodiscard]] bool tick() noexcept;

    // 停止采集并等待音频线程退出（AudioCapture::stop 契约：返回后
    // block_callback 不再被调用）。CaptureSwitchState -> Inactive。
    void stop() noexcept;

    // 生产者空档通知（可选）。switch 事务中「旧流已 stop、新流未 start」的
    // 时刻调用一次，由控制线程执行。
    //
    // 存在的理由：CaptureManager 按设计不触碰 packetizer，但采集端点切换后
    // packetizer 里可能残留属于旧设备的半个 AudioFrame——若不清理，新设备的
    // PCM 会把它补齐，导致一个 AudioFrame 混合两条时间线。清理动作归 runtime
    // 所有（它持有 packetizer），本类只负责在正确的时刻通知。
    //
    // hook 必须快速返回、不得抛异常、不得调用本类的任何方法。
    void set_producer_gap_hook(std::function<void()> hook) noexcept;

    [[nodiscard]] bool available() const noexcept { return capture_ != nullptr; }

    [[nodiscard]] bool is_running() const noexcept
    {
        return capture_ != nullptr && capture_->is_running();
    }

    [[nodiscard]] CaptureSwitchState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] CaptureRouteMode route_mode() const noexcept
    {
        return route_mode_.load(std::memory_order_acquire);
    }

    // 当前请求设备（诊断用）：PreferredDevice 时返回 sticky 配置设备
    // （fallback 降级不覆盖）；FollowSystem 为空。
    [[nodiscard]] std::optional<AudioDeviceId> requested_device() const noexcept
    {
        if (route_mode_.load(std::memory_order_acquire)
            == CaptureRouteMode::PreferredDevice) {
            return preferred_device_;
        }
        return std::nullopt;
    }

    // 当前实际采集设备（成功 start 时缓存：候选解析结果；stop 后 nullopt）。
    // 跟随系统默认用它比较「流当前设备」与「系统当前默认设备」。
    [[nodiscard]] std::optional<AudioDeviceId> active_device() const noexcept
    {
        return active_device_;
    }

    // 最近一次切换事务的结果（对称 PlaybackManager：outcome == None 表示
    // 尚未发生切换事务）。
    [[nodiscard]] std::optional<SwitchResult> last_switch_result() const noexcept
    {
        return last_switch_result_.load(std::memory_order_acquire);
    }

    // 回读采集流实际运行参数（start 成功前 / stop 后为默认值）。
    [[nodiscard]] const AudioCaptureInfo& info() const noexcept;

    // backend 流级诊断（WASAPI 统计；未运行返回零值）。
    [[nodiscard]] AudioCaptureStats stats() const noexcept
    {
        return capture_ != nullptr ? capture_->stats() : AudioCaptureStats { };
    }

private:
    // 回调持有：start() 传入的回调存放于此，restart 复用（对称
    // PlaybackManager::CallbackBundle；MoveOnlyFunction 不可拷贝，
    // 故以 shared_ptr 保活并包装转发）。
    struct CallbackBundle {
        AudioCaptureCallback block;
        AudioCaptureEventCallback event;
    };

    // 以 bundle 包装回调并转发给后端（start/restart 共用）。
    // 候选解析在此完成：device_manager_ 可用时把 optional 请求解析成
    // 具体设备 id 再交给 backend（active_device_ 的记录来源）；解析失败
    // 该候选即失败（如默认设备不存在 -> DeviceNotFound）。
    std::expected<void, AudioError>
    start_stream(const AudioCaptureConfig& route_config,
        const std::shared_ptr<CallbackBundle>& bundle,
        std::optional<AudioDeviceId>& resolved_device) noexcept;

    // 完整切换事务（restart_on_error / tick 跟随共用核心）：
    // 候选链去重、逐项尝试、格式校验、状态与结果维护。
    std::expected<SwitchResult, AudioError>
    switch_to(std::optional<AudioDeviceId> target) noexcept;

    // 共享重试预算（§5 防抖：错误驱动 + 默认变化驱动合并计数）：
    // 窗口内有余量则消费并返回 true；超限返回 false（调用方按链耗尽
    // 处理，进入 Fatal）。
    [[nodiscard]] bool consume_restart_budget() noexcept;

    // previous_active_device：成功 start 时落盘的实际设备（生命周期
    // 状态），其次退回当前请求值。不依赖 backend 实时状态（capture
    // 无设备回读；错误后 resolve 亦可能失败）。
    [[nodiscard]] std::optional<AudioDeviceId> previous_active_device() const noexcept;

    std::unique_ptr<AudioCapture> capture_;
    // 设备系统入口（候选解析 + tick 轮询默认设备）；测试构造可为 nullptr。
    AudioDeviceManager* device_manager_ = nullptr;
    // 生产者空档通知（ServerRuntime 用它清理 packetizer 的半帧残留）。
    // 仅在控制线程调用（switch 事务内），不需要与查询同步。
    std::function<void()> producer_gap_hook_;
    std::shared_ptr<CallbackBundle> callbacks_;
    // 路由配置（source + 请求 device + 钉死的会话 format + buffer 参数）。
    AudioCaptureConfig active_config_ { };
    // 最近一次成功 start 的实际设备（候选解析结果；stop() 清空）。
    std::optional<AudioDeviceId> active_device_;
    // sticky 用户意图：PreferredDevice 的目标设备（= CLI 配置值）。
    // fallback 降级（active_config_.device 被覆写为兜底设备）不影响它。
    std::optional<AudioDeviceId> preferred_device_;
    std::atomic<CaptureSwitchState> state_ { CaptureSwitchState::Inactive };
    std::atomic<CaptureRouteMode> route_mode_ { CaptureRouteMode::FollowSystem };
    std::atomic<SwitchResult> last_switch_result_ { };

    // 重试窗口（仅控制线程访问，与生命周期方法同线程串行）：
    // 所有自动 restart 在窗口内最多 kMaxAutoRestarts 次（§5）。
    static constexpr auto kRetryWindow = std::chrono::seconds(10);
    static constexpr unsigned kMaxAutoRestarts = 3;
    std::chrono::steady_clock::time_point window_start_
        = std::chrono::steady_clock::now();
    unsigned auto_restarts_in_window_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_CAPTURE_CAPTURE_MANAGER_H
