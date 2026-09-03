#ifndef AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
#define AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H

// PlaybackManager：播放生命周期的管理边界（doc/playback_switching_design.md）。
//
// 层级：ClientRuntime --> PlaybackManager --> AudioPlayback --> AAudio/WASAPI
//
// 职责边界（防止切换策略下沉到 backend）：
//   - PlaybackManager：active device、switching 状态、stop/start 顺序、
//     rollback、诊断；
//   - AudioPlayback：打开设备、创建流、callback 生命周期、backend error 转换。
// AudioPlayback 不提供 switch_device 之类的策略 API；restart 事务（同设备或
// 换设备）全部在 PlaybackManager 内编排为 stop -> start 序列。
//
// restart 事务链（playback_switching_design.md §5）：
//   Switching -> 捕获 previous_active_device（stream_info 回读）-> stop 旧流
//   （同步 join 回调线程，AudioPlayback::stop 契约保证返回后旧回调不再访问
//   JitterBuffer）-> 依次尝试去重候选 [target, previous, system_default]
//   -> 首个成功者 Running；链耗尽 -> Fatal（终态，supervision 将 stop runtime）。
//   全程不触碰 JitterBuffer / playhead / 诊断计数。
//
// 防抖与重试上限（§5）：错误驱动的自动 restart（restart_on_error）在 10s
// 窗口内最多 3 次，超过按链耗尽处理（防蓝牙连接风暴造成重启死循环）。
// 用户显式选择（set_playback_device）不计数并重置窗口。
//
// 线程约定：start/restart/set_playback_device/restart_on_error/stop/
// on_devices_changed 必须由同一控制线程串行调用（与 ClientRuntime 生命周期
// 路径一致）；查询（state/stream_info/route_mode/last_switch_result）任意线程。

#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_route_mode.h"
#include "aqua/audio/playback/playback_state.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace aqua::audio {

// 切换结果：成功路径的降级信息（playback_switching_design.md §9）。
enum class SwitchOutcome : std::uint8_t {
    None, // 尚未发生切换事务（start 后的初始状态）
    Switched, // 目标设备（或同设备 restart）一次成功
    RolledBack, // 目标失败，回滚 previous_active_device 成功
    FellBackToSystem, // 目标与回滚均失败，落系统默认成功
    Fatal, // 候选链耗尽（格式不兼容 / 重试超限）
};

struct SwitchResult {
    SwitchOutcome outcome = SwitchOutcome::None;
    AudioError last_error = AudioError::None; // 链上最后一次失败原因
};

class PlaybackManager final {
public:
    // 创建平台回放后端；平台不支持时 available() == false。
    explicit PlaybackManager(AudioDeviceManager& device_manager);

    // 直接注入后端实例（测试用；生产路径走上面的工厂构造）。
    explicit PlaybackManager(std::unique_ptr<AudioPlayback> playback);

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    // 启动回放：转发 AudioPlayback::start，并维护 PlaybackState
    // （Starting -> Running；失败回 Inactive）。
    // 成功后记住 config 与回调（restart 复用）；回调经 shared bundle 保活，
    // AudioPlaybackCallback 不可拷贝，restart 需要重新传入同一回调。
    // 初始路由模式由 config.device 推导：nullopt -> FollowSystem，
    // 有值 -> PreferredDevice。prefer_current_on_start（见下）可覆盖
    // nullopt 分支为 PreferCurrent。
    std::expected<void, AudioError>
    start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept;

    // 连接起步路由覆盖（playback_switching_design.md §4）："自动切换播放
    // 设备"关的会话以 PreferCurrent 起步——首流成功后把实际设备（stream_info
    // 回读）钉进 active_config，错误驱动 restart 锚定该设备而不跟随新的
    // 系统默认。必须在 start() 前调用，只影响下一次 start()。
    void set_prefer_current_on_start(bool hold) noexcept
    {
        prefer_current_on_start_ = hold;
    }

    // 同设备 restart（A-0 语义）：stop 旧流 -> 以同一 config 重新 start。
    // 前置：此前 start() 成功过（否则 NotRunning）。
    // 失败（无 fallback 链）：PlaybackState -> Inactive 并返回错误。
    std::expected<void, AudioError> restart() noexcept;

    // 显式切换目标设备（用户选择；nullopt = 跟随系统）。
    // 走完整候选链（target -> previous -> system_default 去重）；
    // 成功后更新路由模式（有 id -> PreferredDevice，nullopt -> FollowSystem）。
    // 用户显式选择不计数并重置重试窗口。
    // 链耗尽：PlaybackState -> Fatal 并返回链上最后一个错误。
    std::expected<SwitchResult, AudioError>
    set_playback_device(std::optional<AudioDeviceId> target) noexcept;

    // 错误驱动的自动 restart（设备拔出 / 流断开等）：
    // 按当前路由模式推导目标（FollowSystem -> nullopt；PreferCurrent ->
    // 当前实际设备；PreferredDevice -> 当前请求设备），走同一候选链。
    // 受重试上限约束（10s 窗口最多 3 次），超限直接 Fatal（不触碰后端）。
    // 不改变路由模式（fallback 是临时降级，用户意图不动）。
    std::expected<SwitchResult, AudioError> restart_on_error() noexcept;

    // 停止回放并等待回调线程退出（AudioPlayback::stop 契约：返回后
    // callback 不再被调用）。PlaybackState -> Inactive。
    void stop() noexcept;

    [[nodiscard]] bool available() const noexcept { return playback_ != nullptr; }

    [[nodiscard]] bool is_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

    [[nodiscard]] PlaybackState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] PlaybackRouteMode route_mode() const noexcept
    {
        return route_mode_.load(std::memory_order_acquire);
    }

    // 当前请求设备（诊断用）：PreferredDevice 时返回 sticky 用户意图
    // （preferred_device_，fallback 降级不覆盖）；PreferCurrent 时返回钉住的
    // 实际设备；FollowSystem 为空。
    [[nodiscard]] std::optional<AudioDeviceId> requested_device() const noexcept
    {
        if (route_mode_.load(std::memory_order_acquire)
            == PlaybackRouteMode::PreferredDevice) {
            return preferred_device_;
        }
        return active_config_.device;
    }

    // 当前实际输出设备（成功 start 时缓存；stop 后 nullopt）。系统默认设备
    // 变化跟随用它比较「流当前设备」与「系统当前默认设备」。控制线程
    // （lifecycle 串行路径）读取；诊断近似读亦可容忍。
    [[nodiscard]] std::optional<AudioDeviceId> active_device() const noexcept
    {
        return active_device_;
    }

    // 最近一次切换事务的结果（start 后为 Switched/None；从未切换 = nullopt）。
    [[nodiscard]] std::optional<SwitchResult> last_switch_result() const noexcept
    {
        return last_switch_result_.load(std::memory_order_acquire);
    }

    // 回读输出流实际运行参数（start 成功前 / stop 后 backend=None）。
    [[nodiscard]] AudioStreamInfo stream_info() const noexcept
    {
        return playback_ != nullptr ? playback_->stream_info() : AudioStreamInfo { };
    }

    // 路由状态轮询（由 ClientRuntime 的 supervision tick 每 500ms 调用，已在
    // lifecycle 串行路径内）：仅 FollowSystem 模式查询系统默认输出设备，若
    // 与当前实际设备不同则 set_playback_device(nullopt) 跟随。设备查询与切换
    // 决策都收敛在本类（持 AudioDeviceManager 引用），不污染 backend 与 runtime。
    void tick() noexcept;

    // 设备集合变化事件（平台推送模型，playback_switching_design.md §5 rev2）：
    // Android 由 Kotlin AudioManager 回调经 C API 转发（设备发现留在 Kotlin，
    // core 不建注册表，只消费事件快照）。present = 当前可选输出设备 id 全集
    // （后端词汇，如 "android:N"）。由控制线程串行调用（lifecycle 路径内）。
    //
    // 决策（全部由本类完成，调用方只转发事件）：
    //   - 活跃设备不在集合 → 按路由模式 eager restart（restart_on_error 路径：
    //     路由推导目标 + fallback 链 + 重试预算；保留 route mode）；
    //   - FollowSystem 且有新增设备 → 跟随系统默认（set_playback_device(nullopt)，
    //     新设备通常已成为系统默认输出）；
    //   - PreferredDevice 且请求设备回归（当前不在其上）→ 自动切回
    //     （proactive，不占错误重试预算；失败回滚后用户意图仍保留，下次
    //     设备再次出现时可重试）；
    //   - PreferCurrent → 仅活跃设备消失时动作，其余不动作。
    // 每份连接的首份快照只作基线记录，不触发决策（避免连接初期的初始
    // 设备列表被误判为"新增设备"）。
    //
    // 返回 true = 本次事件触发了切换事务（ClientRuntime 据此吸收待处理的
    // 设备错误标志，避免与错误驱动恢复双重 restart）。
    bool on_devices_changed(const std::vector<AudioDeviceId>& present) noexcept;

private:
    // 回调持有：start() 传入的回调存放于此，restart 复用。
    // MoveOnlyFunction 不可拷贝，故以 shared_ptr 保活并包装转发。
    struct CallbackBundle {
        AudioPlaybackCallback pull;
        AudioPlaybackEventCallback event;
    };

    // 以 bundle 包装回调并转发给后端（start/restart 共用）。
    std::expected<void, AudioError>
    start_stream(const AudioPlaybackConfig& config,
        const std::shared_ptr<CallbackBundle>& bundle) noexcept;

    // 完整切换事务（set_playback_device / restart_on_error 共用核心）：
    // 前置已检查；负责候选链去重、逐项尝试、状态与结果维护。
    std::expected<SwitchResult, AudioError>
    switch_to(std::optional<AudioDeviceId> target) noexcept;

    // 成功 start 后把「实际输出设备」缓存进 active_device_（优先 stream_info
    // 回读，回读为空退回请求值）。previous_active_device 以此为准。
    void cache_active_device(const std::optional<AudioDeviceId>& requested) noexcept;

    // previous_active_device：优先 active_device_（成功 start 时落盘的
    // 生命周期状态），其次 stream_info 实时回读，最后退回请求值。
    // 缓存优先使切换/恢复不依赖 backend 当前（可能已 error/stop 清零）回读。
    [[nodiscard]] std::optional<AudioDeviceId> previous_active_device() const noexcept;

    std::unique_ptr<AudioPlayback> playback_;
    // 设备系统入口（tick() 轮询默认设备用）；测试构造（注入 backend）为 nullptr。
    AudioDeviceManager* device_manager_ = nullptr;
    std::shared_ptr<CallbackBundle> callbacks_;
    AudioPlaybackConfig active_config_ { };
    // 最近一次成功 start 的实际输出设备（成功时缓存，stop() 清空）。
    // previous_active_device 优先读它，避免依赖 backend stream_info 的实时状态。
    std::optional<AudioDeviceId> active_device_;
    // sticky 用户意图：PreferredDevice 的目标设备。set_playback_device(id)
    // 记入，set_playback_device(nullopt) 清除；fallback 降级（active_config_
    // 被覆写为兜底设备）不影响它——这是"优先而非固定"语义的载体，也是
    // 自动切回（on_devices_changed）与错误驱动 restart 的目标来源。
    std::optional<AudioDeviceId> preferred_device_;
    // 设备事件基线（on_devices_changed）：上一份工作快照；valid=false 时
    // 下一份快照只记录不决策（连接初期基线）。仅控制线程访问。
    std::vector<AudioDeviceId> known_devices_;
    bool known_devices_valid_ = false;
    // 连接起步路由覆盖（set_prefer_current_on_start；仅 start() 读取）。
    bool prefer_current_on_start_ = false;
    std::atomic<PlaybackState> state_ { PlaybackState::Inactive };
    std::atomic<PlaybackRouteMode> route_mode_ { PlaybackRouteMode::FollowSystem };
    std::atomic<SwitchResult> last_switch_result_ { };

    // 重试窗口（仅控制线程访问，与生命周期方法同线程串行）：
    // 错误驱动 restart 在窗口内最多 kMaxErrorRestarts 次。
    static constexpr auto kRetryWindow = std::chrono::seconds(10);
    static constexpr unsigned kMaxErrorRestarts = 3;
    std::chrono::steady_clock::time_point window_start_
        = std::chrono::steady_clock::now();
    unsigned error_restarts_in_window_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
