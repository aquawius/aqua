#ifndef AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
#define AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H

// PlaybackManager：播放生命周期的管理边界（doc/playback_switching_design.md）。
//
// 层级：ClientRuntime --> PlaybackManager --> AudioPlayback --> AAudio/WASAPI
//
// 职责边界（防止切换策略下沉到 backend）：
//   - PlaybackManager：持有 RoutePolicy、active device、switching 状态、
//     stop/start 顺序、rollback、重试预算、诊断；
//   - AudioPlayback：按 AudioRoute 打开一条流、callback 生命周期、backend
//     error 转换，并给每个事件打上 stream generation。
// AudioPlayback 不提供 switch_device 之类的策略 API；restart 事务（同设备或
// 换设备）全部在 PlaybackManager 内编排为 stop -> start 序列。
//
// 路由模型（两根正交的轴，见 devices/audio_route.h 与 devices/route_policy.h）：
//   AudioRoute  = 给 backend 的请求：跟随系统 / 绑定到具体端点；
//   RoutePolicy = 本类的决策状态：意图归谁（None/Application/User）+
//                 钉住的设备丢失时怎么办。
// client 侧的丢失动作恒为 FallbackToSystem（移动端「永不主动静音」），与
// server 采集侧的 Fatal 相反——这个差别由 RouteLossAction 表达，不再靠
// 两个同名不同义的 RouteMode 枚举隐含。
//
// restart 事务链（playback_switching_design.md §5）：
//   Switching -> 捕获 previous_active_device（stream_info 回读）-> stop 旧流
//   （同步 join 回调线程，AudioPlayback::stop 契约保证返回后旧回调不再访问
//   JitterBuffer）-> 依次尝试去重候选 [target, previous, system_default]
//   -> 首个成功者 Running；链耗尽 -> 先对"刚关闭的上一设备"做有界重试
//   （仅瞬时错误，见 playback_manager.cpp 顶部 kSwitchRetry*：移动端的设备摘除
//   是异步的，且系统默认此刻常与 previous 同一落点，三层链会退化成一层），
//   重试成功即 RolledBack（会话保住），仍失败才 Fatal（终态，supervision 将
//   stop runtime）。全程不触碰 JitterBuffer / playhead / 诊断计数。
//
// 两类失败必须分开计数（§5 rev3）：
//   设备丢失    —— 流跑了一段时间才死。走 kMaxErrorRestarts（10s/3）预算，
//                  超限 Fatal。
//   路由未稳定  —— 流刚起来就死（!is_running() 且年龄 < kRouteSettleWindow）。
//                  Android 的 AudioPolicy 在设备转换期间会重新路由并把
//                  DISCONNECTED 投递给**当前**流；此时立刻重开只是再次触发同
//                  一次路由变更。走独立的 settle 预算 + 最小重试间隔，绝不占
//                  用设备丢失预算。
// 把两者混在一个预算里，会让一次 250ms 的路由抖动烧光名义上「10 秒 3 次」的
// 额度并直接 Fatal（实测：4 次尝试全挤在 256ms 内，会话被断）。
//
// 线程约定：start/restart/set_playback_device/restart_on_error/stop/
// on_devices_changed 必须由同一控制线程串行调用（与 ClientRuntime 生命周期
// 路径一致）；查询（state/stream_info/intent_owner/last_switch_result）任意线程。

#include "aqua/audio/audio_switch_result.h"
#include "aqua/audio/audio_stream_event.h"
#include "aqua/audio/devices/device_set_poller.h"
#include "aqua/audio/devices/route_policy.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_state.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace aqua::audio {

// SwitchOutcome / SwitchResult 已移至 aqua/audio/audio_switch_result.h
// （capture 侧 CaptureManager 共享同一结果词汇）。

class PlaybackManager final {
public:
    // client 侧丢失动作：永不主动静音，钉住的设备不可用就降级到系统默认。
    static constexpr RouteLossAction kLossAction = RouteLossAction::FallbackToSystem;

    // 创建平台回放后端；平台不支持时 available() == false。
    explicit PlaybackManager(AudioDeviceManager& device_manager);

    // 直接注入后端实例（测试用；生产路径走上面的工厂构造）。
    // device_manager 非空时同时启用 tick 的设备集合轮询（对称 CaptureManager
    // 的同形构造）；为空则 tick 跳过全部轮询。
    explicit PlaybackManager(std::unique_ptr<AudioPlayback> playback,
        AudioDeviceManager* device_manager = nullptr);

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    // 启动回放：转发 AudioPlayback::start，并维护 PlaybackState
    // （Starting -> Running；失败回 Inactive）。
    // 成功后记住 config 与回调（restart 复用）；回调经 shared bundle 保活，
    // AudioPlaybackCallback 不可拷贝，restart 需要重新传入同一回调。
    // 初始 RoutePolicy 由 config.route 推导：pin -> User（sticky）；
    // 跟随系统 -> None，prefer_current_on_start（见下）可把它升级为
    // Application（钉住首流实际落点）。
    std::expected<void, AudioError>
    start(const AudioPlaybackConfig& config,
        AudioPlaybackCallback callback,
        AudioPlaybackEventCallback event_callback = { }) noexcept;

    // 连接起步路由覆盖（playback_switching_design.md §4）："自动切换播放
    // 设备"关的会话，首流成功后把实际设备（stream_info 回读）钉进
    // active_config，错误驱动 restart 锚定该设备而不跟随新的系统默认。
    // 必须在 start() 前调用，只影响下一次 start()。
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
    // 成功后更新 RoutePolicy（有 id -> User sticky，nullopt -> None）。
    // 用户显式选择不计入任何预算并重置窗口。
    // 链耗尽：PlaybackState -> Fatal 并返回链上最后一个错误。
    std::expected<SwitchResult, AudioError>
    set_playback_device(std::optional<AudioDeviceId> target) noexcept;

    // 错误驱动的自动 restart（设备拔出 / 流断开等）：
    // 目标由 RoutePolicy::restart_target 推导，走同一候选链。
    //
    // 预算分流（见文件头）：当前流**真的死了**（!is_running()）且死在
    // kRouteSettleWindow 内 = 路由未稳定，走 settle 预算并受
    // kSettleRetryInterval 节流；节流命中时**不改任何状态**直接返回错误，由
    // supervision tick 稍后驱动（那时 backend 已 is_running()==false 而 state
    // 仍为 Running，正好落入 ClientRuntime 的 silent-death 兜底分支）。
    // 其余情况（含流还活着的显式 restart 请求）才走 kMaxErrorRestarts。
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

    // 意图归属（跨线程诊断投影；完整 RoutePolicy 含 std::string，仅控制线程
    // 可读）。取值即 aqua_route_mode 编码。
    [[nodiscard]] RouteIntentOwner intent_owner() const noexcept
    {
        return intent_owner_.load(std::memory_order_acquire);
    }

    // 完整路由策略。含 sticky intent（std::string），只允许控制线程读取
    // （ClientRuntime 在 lifecycle_mutex_ 内调用）。
    [[nodiscard]] RoutePolicy route_policy() const noexcept
    {
        RoutePolicy policy { intent_owner(), kLossAction, std::nullopt };
        policy.intent = intent_;
        return policy;
    }

    // 诊断用设备意图：User（sticky）时返回用户意图（fallback 降级不覆盖）；
    // 其余返回当前会话实际配置的设备——Application 下是启动时钉住的设备，
    // None 起步为空、发生过一次切换后是切过去的设备。
    //
    // 与 CaptureManager::preferred_device() 不同名是有意的：采集侧在非 User
    // 归属下一律返回 nullopt（没有钉住实际设备的语义），两者行为不对称，
    // 不该共用 request 系的名字。
    [[nodiscard]] std::optional<AudioDeviceId> preferred_or_active_device() const noexcept
    {
        if (intent_owner() == RouteIntentOwner::User) {
            return intent_;
        }
        return active_config_.route.endpoint_request();
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

    // 切换事务序号：每笔 switch_to（成功 / 回滚 / 兜底 / 链耗尽都算）递增一次。
    // 为什么需要：诊断里只有"最近一次结果"（outcome/error/duration），两笔不同
    // 的事务可能给出**完全相同**的结果值——典型是"手动 pin 蓝牙"与"蓝牙断开后
    // 自动回落扬声器"都是 Switched + None，UI 用"结果变化"当提示判据就会把后
    // 一笔整个吞掉（V0.2.2 起的老问题）。序号让"又发生了一次切换"成为可判定的
    // 事实；设备 id 在部分平台回读为空（AAudio UNSPECIFIED），不能当唯一判据。
    [[nodiscard]] std::uint32_t switch_seq() const noexcept
    {
        return switch_seq_.load(std::memory_order_acquire);
    }

    // 回读输出流实际运行参数（start 成功前 / stop 后 backend=None）。
    [[nodiscard]] AudioStreamInfo stream_info() const noexcept
    {
        return playback_ != nullptr ? playback_->stream_info() : AudioStreamInfo { };
    }

    // 因 generation 不匹配而被丢弃的事件数（旧流的迟到讣告 / teardown 期间的
    // 临终事件）。非零说明 provenance 过滤真的拦下了东西——这是判断"事件归属"
    // 契约有没有在起作用的唯一可观测量。
    [[nodiscard]] std::uint64_t stale_events_dropped() const noexcept
    {
        return stale_events_dropped_.load(std::memory_order_relaxed);
    }

    // 路由状态轮询（由 ClientRuntime 的 supervision tick 每 500ms 调用，已在
    // lifecycle 串行路径内）。两步，都收敛在本类（持 AudioDeviceManager 引用），
    // 不污染 backend 与 runtime：
    //   1. 设备集合轮询（**所有归属**）：DeviceSetPoller 去抖后把确认过的集合
    //      交给 on_devices_changed 决策。Windows 上这是"钉住设备回归自动切回"
    //      的唯一触发源——notify_devices_changed 只有 JNI 调用者，而本函数原先
    //      对 owner != None 直接返回，于是 client 钉住的设备拔掉后能回落（流
    //      错误事件驱动），插回来却永远不切回。Android 的 enumerate 只给合成
    //      条目，poll() 恒返回 nullopt，决策仍由推送驱动；两条路径共用
    //      on_devices_changed，互不干扰。
    //   2. 仅跟随系统时比较系统默认输出设备，与当前实际设备不同则内部跟随
    //      （follow_system_default：消费重试预算，不碰用户意图与路由策略）。
    //      其它归属下用户/App 意图优先，不查询也不跟随。
    // 返回 true = 本次 tick 执行了切换事务（ClientRuntime 据此吸收待处理的
    // 设备错误标志，避免与错误驱动恢复双重 restart）。
    [[nodiscard]] bool tick() noexcept;

    // 设备集合变化事件（playback_switching_design.md §5 rev2）。两个调用源，
    // 决策逻辑同一份：
    //   - 推送：Android 由 Kotlin AudioManager 回调经 C API 转发（设备发现留在
    //     Kotlin，core 不建注册表，只消费事件快照）；
    //   - 轮询：tick() 经 DeviceSetPoller 去抖后转发（Windows 等无推送的平台）。
    // present = 当前可选输出设备 id 全集（后端词汇，如 "android:N" / WASAPI
    // endpoint id）。由控制线程串行调用（lifecycle 路径内）。
    //
    // 决策（全部由本类完成，调用方只转发事件）：
    //   - 活跃设备不在集合 → 按路由策略 eager restart（restart_on_error 路径：
    //     策略推导目标 + fallback 链 + 重试预算；保留 RoutePolicy）；
    //   - 跟随系统且有新增设备 → 内部跟随系统默认（follow_system_default；
    //     默认可查询且确实变化时才重开，否则跳过——tick 会兜底真变化）；
    //   - User 归属且意图设备回归（当前不在其上）→ 自动切回（proactive，
    //     同样消费重试预算；失败回滚后用户意图仍保留，下次设备再次出现时
    //     可重试）；
    //   - Application 归属 → 仅活跃设备消失时动作，其余不动作。
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
    // 事件在此按 generation 过滤：只放行 active_generation_ 的事件，旧流的
    // 迟到讣告计数后丢弃（provenance 契约的 manager 侧落点）。
    std::expected<void, AudioError>
    start_stream(const AudioPlaybackConfig& config,
        const std::shared_ptr<CallbackBundle>& bundle) noexcept;

    // 完整切换事务（set_playback_device / restart_on_error / 内部跟随共用核心）：
    // 前置已检查；负责候选链去重、逐项尝试、状态与结果维护。
    // 候选链形态由 target 决定：显式 target 走完整 fallback 链
    // [target, previous, system_default]；nullopt（自动/用户跟随）单候选
    // 直达当前默认，不回滚 previous（跟随语义下旧设备正是要离开的）。
    std::expected<SwitchResult, AudioError>
    switch_to(std::optional<AudioDeviceId> target) noexcept;

    // 内部自动跟随（tick 轮询 / 快照新增驱动）：目标恒为 nullopt（当前默认），
    // 与错误驱动共享重试预算（configuration_reference §4），耗尽即 Fatal；
    // 不碰 sticky 用户意图与路由策略（调用方已处于跟随系统）。
    std::expected<SwitchResult, AudioError> follow_system_default() noexcept;

    // 设备丢失预算（10s/kMaxErrorRestarts）：错误驱动 restart 与内部自动跟随
    // 共享；用户显式 set_playback_device 不经此处（直接重置窗口）。
    // 耗尽时落 Fatal 终态并返回 false。
    bool consume_restart_budget() noexcept;

    // 路由未稳定预算（kSettleWindow/kMaxSettleRestarts）+ 最小重试间隔。
    // 返回 false = 本次不重试（节流中，状态不变，交给 supervision tick）或
    // settle 预算耗尽（已落 Fatal）。与设备丢失预算完全独立。
    bool consume_settle_budget() noexcept;

    // 写入路由策略并同步跨线程诊断投影（唯一写者）。
    void set_policy(RoutePolicy policy) noexcept;

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

    // ---- 路由策略（取代原先并行的 route_mode_ + preferred_device_）----
    // intent_owner_ 是跨线程诊断投影；intent_ 是 sticky 用户/App 意图，含
    // std::string 故仅控制线程访问。两者只由 set_policy() 写入。
    std::atomic<RouteIntentOwner> intent_owner_ { RouteIntentOwner::None };
    std::optional<AudioDeviceId> intent_;
    // 连接起步路由覆盖（set_prefer_current_on_start；仅 start() 读取）。
    bool prefer_current_on_start_ = false;

    // ---- 流归属（provenance）----
    // backend 每次成功 start() 递增 generation；事件带自己的 generation。
    // active_generation_ 是本类认可的「当前流」，事务开始时先置为
    // kNoStreamGeneration，使 teardown 期间的事件在 manager 侧即被丢弃
    // （不再依赖 ClientRuntime 的 Switching 时间窗猜测）。
    std::atomic<StreamGeneration> active_generation_ { kNoStreamGeneration };
    // 最近一次成功 start 的时刻，用于把「刚起来就死」判为路由未稳定。
    std::chrono::steady_clock::time_point stream_started_at_ { };
    std::atomic<std::uint64_t> stale_events_dropped_ { 0 };

    // 设备事件基线（on_devices_changed）：上一份工作快照；valid=false 时
    // 下一份快照只记录不决策（连接初期基线）。仅控制线程访问。
    std::vector<AudioDeviceId> known_devices_;
    bool known_devices_valid_ = false;
    // 设备集合轮询（tick 第 1 步）。仅 device_manager 构造时创建；测试构造
    // （注入 backend）为 nullopt，tick 跳过轮询。与 known_devices_ 同为控制
    // 线程状态，start() 一并 reset。
    std::optional<DeviceSetPoller> device_poller_;
    std::atomic<PlaybackState> state_ { PlaybackState::Inactive };
    std::atomic<SwitchResult> last_switch_result_ { };
    // 切换事务序号（每笔 switch_to 递增，供诊断/UI 判定"又切了一次"）。
    std::atomic<std::uint32_t> switch_seq_ { 0 };

    // 设备丢失重试窗口（仅控制线程访问，与生命周期方法同线程串行）：
    // 错误驱动 restart 与内部自动跟随（tick/快照/自动切回）在窗口内合计
    // 最多 kMaxErrorRestarts 次；用户显式 set_playback_device 重置窗口。
    static constexpr auto kRetryWindow = std::chrono::seconds(10);
    static constexpr unsigned kMaxErrorRestarts = 3;
    std::chrono::steady_clock::time_point window_start_
        = std::chrono::steady_clock::now();
    unsigned error_restarts_in_window_ = 0;

    // ---- 路由未稳定（settle）预算 ----
    // 实测依据（temp/android_switch.log）：蓝牙 -> 内建扬声器切换时，每条新流
    // 在 requestStart 成功后 6~8ms 收到 AAUDIO_ERROR_DISCONNECTED，四次尝试全
    // 挤在 256ms 内，把 10s/3 的设备丢失预算烧光后 Fatal，supervision 随即停
    // 掉整个 ClientRuntime（用户观感「换个设备把连接搞断了」）。同一日志里
    // 4.6s / 614ms 无 close 的自发 DISCONNECTED 证明这些事件属于**当前**流，
    // 不是旧流的迟到讣告——所以修法是给路由留时间，而不是过滤事件归属。
    //
    // kRouteSettleWindow 取值：一次 A2DP <-> 扬声器 的 AudioPolicy 转换在数百
    // 毫秒量级（同日志中一次成功的切换耗时 278ms），400ms 足以覆盖「刚起来就
    // 死」而不会把真实的设备丢失误判为抖动。
    static constexpr auto kRouteSettleWindow = std::chrono::milliseconds(400);
    // 节流间隔：让重试跨越路由转换窗口。不在 ioc 线程上 sleep——节流命中即
    // 返回，由 500ms 的 supervision tick 驱动下一次尝试。
    static constexpr auto kSettleRetryInterval = std::chrono::milliseconds(200);
    static constexpr auto kSettleWindow = std::chrono::seconds(5);
    static constexpr unsigned kMaxSettleRestarts = 8;
    std::chrono::steady_clock::time_point settle_window_start_
        = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_settle_attempt_ { };
    unsigned settle_restarts_in_window_ = 0;
};

} // namespace aqua::audio

#endif // AQUA_AUDIO_PLAYBACK_PLAYBACK_MANAGER_H
