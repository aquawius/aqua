#include "aqua/audio/playback/playback_manager.h"

#include "aqua/audio/audio_switch_result.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace aqua::audio {
namespace {

    // 候选 -> 路由请求：有 id 即绑定，nullopt 即交还路由权给平台。
    [[nodiscard]] AudioRoute candidate_route(
        const std::optional<AudioDeviceId>& candidate)
    {
        return candidate ? AudioRoute::pin(*candidate) : AudioRoute::follow_system();
    }

    // 切换失败是否为"瞬时类"：设备正在被异步摘除 / 尚未就绪 / 后端忙。
    // 只有这类错误值得重试——格式、参数、权限类错误重试无意义，只会把 Fatal
    // 推迟几百毫秒并污染 start 计数。
    [[nodiscard]] constexpr bool is_transient_switch_error(AudioError error) noexcept
    {
        return error == AudioError::DeviceUnavailable
            || error == AudioError::DeviceDisconnected
            || error == AudioError::BackendFailed;
    }

} // namespace

// ---- 切换事务的瞬时失败重试（break-before-make 的代价）----
// switch_to 先 stop 再 start；移动端的 A2DP / USB 音频摘除是**异步**的，刚
// 关闭的上一设备常常在几百毫秒内重开失败。而系统默认此刻往往仍是同一台设备
// （nullopt 与 previous 解析到同一落点），于是 [target, previous, nullopt]
// 三层链实际退化成一层——一次瞬时失败就直达 Fatal，用户观感是"换个设备把整
// 条连接搞断了"。下面的有界重试只在这种情形兜底，能回到上一设备即保住会话。
constexpr unsigned kSwitchRetryAttempts = 4;
constexpr unsigned kSwitchRetryBackoffMs = 200;

PlaybackManager::PlaybackManager(AudioDeviceManager& device_manager)
    : playback_(create_playback(device_manager))
    , device_manager_(&device_manager)
{
    if (!playback_) {
        log_error("PlaybackManager: audio playback backend is unavailable on this platform");
    }
    // 输出方向恒定，故在构造期即可建轮询器（capture 侧方向取决于 config.source，
    // 只能在 start() 建）。
    device_poller_.emplace(device_manager, AudioDeviceDirection::OUTPUT);
}

PlaybackManager::PlaybackManager(std::unique_ptr<AudioPlayback> playback,
    AudioDeviceManager* device_manager)
    : playback_(std::move(playback))
    , device_manager_(device_manager)
{
    // 测试构造：注入 backend（生产路径走上面的工厂构造），设备入口可选。
    if (device_manager_ != nullptr) {
        device_poller_.emplace(*device_manager_, AudioDeviceDirection::OUTPUT);
    }
}

void PlaybackManager::set_policy(RoutePolicy policy) noexcept
{
    intent_ = std::move(policy.intent);
    // 诊断投影与 sticky 意图在同一次写入内保持一致；跨线程读者只看到 owner。
    intent_owner_.store(policy.owner, std::memory_order_release);
}

std::expected<void, AudioError> PlaybackManager::start_stream(
    const AudioPlaybackConfig& config,
    const std::shared_ptr<CallbackBundle>& bundle) noexcept
{
    // 包装转发：lambda 持有 bundle 的 shared_ptr 引用（AudioPlaybackCallback
    // 不可拷贝；包装后 restart 可重复传入同一回调）。
    auto wrapped_pull = [bundle](std::span<std::byte> output) noexcept {
        return bundle->pull(output);
    };
    AudioPlaybackEventCallback wrapped_event;
    if (bundle->event) {
        // provenance 契约的 manager 侧落点：只放行当前 generation 的事件。
        // 事务开始时 active_generation_ 已被置为 kNoStreamGeneration，因此
        // teardown 期间 backend 投递的旧流临终错误在此即被丢弃，不再依赖
        // ClientRuntime 用 PlaybackState::Switching 猜时间窗。
        // 本 lambda 可能运行在后端的实时线程上（AAudio data callback 的异常
        // 路径经 report_fatal_once 调用），因此只计数不打日志。
        wrapped_event = [bundle, this](const AudioStreamEvent& event) noexcept {
            if (event.generation != active_generation_.load(std::memory_order_acquire)) {
                stale_events_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            bundle->event(event);
        };
    }
    const auto result = playback_->start(config, std::move(wrapped_pull), std::move(wrapped_event));
    if (!result) {
        return result;
    }
    // 认领这条流：此后只有它的事件能穿过上面的过滤。
    active_generation_.store(playback_->generation(), std::memory_order_release);
    stream_started_at_ = std::chrono::steady_clock::now();
    return result;
}

std::expected<void, AudioError> PlaybackManager::start(
    const AudioPlaybackConfig& config,
    AudioPlaybackCallback callback,
    AudioPlaybackEventCallback event_callback) noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callback) {
        // 后端只见到非空的包装回调，空回调校验收敛在 manager。
        return std::unexpected(AudioError::InvalidArgument);
    }

    auto bundle = std::make_shared<CallbackBundle>();
    bundle->pull = std::move(callback);
    bundle->event = std::move(event_callback);

    state_.store(PlaybackState::Starting, std::memory_order_release);
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);
    const auto result = start_stream(config, bundle);
    if (!result) {
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        return result;
    }
    active_config_ = config;
    callbacks_ = std::move(bundle);
    cache_active_device(config.route.endpoint_request());
    known_devices_.clear();
    known_devices_valid_ = false;
    if (device_poller_) {
        device_poller_->reset();
    }

    // 路由策略（playback_switching_design.md §4）：显式 pin -> User（sticky，
    // 设备回归自动切回）；跟随系统 -> None，除非本连接要求 prefer_current
    // （"自动切换播放设备"关）——那时把首流实际落点钉成 Application 意图，
    // 后续错误驱动 restart 锚定它而不跟随新的系统默认。后端读不到实际落点
    // （device_id 为空）时保持 None，退化为跟随系统的 restart 语义。
    auto policy = RoutePolicy::from_route(config.route, kLossAction);
    if (policy.follows_system() && prefer_current_on_start_) {
        const auto actual = playback_->stream_info().device_id;
        if (!actual.empty()) {
            active_config_.route = AudioRoute::pin(actual);
            policy = RoutePolicy::app_pinned(AudioDeviceId(actual), kLossAction);
        }
    }
    set_policy(std::move(policy));

    // 两套预算随新会话重置（用户意图与设备事件基线同理）。
    const auto now = std::chrono::steady_clock::now();
    error_restarts_in_window_ = 0;
    window_start_ = now;
    settle_restarts_in_window_ = 0;
    settle_window_start_ = now;
    last_settle_attempt_ = { };
    last_switch_result_.store(SwitchResult { }, std::memory_order_release);
    state_.store(PlaybackState::Running, std::memory_order_release);
    // 与 CaptureManager::start 对称：一次性打出本条会话的路由**决策状态**。
    // backend 的 "WASAPI playback started: device=..." 只说明流开在哪，看不出
    // owner/on_loss，也就无法从日志区分"跟随系统"与"用户钉住"——而后续所有
    // 自动行为（是否跟随默认、设备回归是否切回、丢失是否 Fatal）都由它决定。
    // prefer_current 会把首流落点钉成 Application，因此必须在 set_policy 之后打。
    const auto info = stream_info();
    log_info_fmt("PlaybackManager started: route={} on_loss={} device={} format={}ch/{}Hz/enc={}",
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)),
        route_loss_action_name(kLossAction),
        active_device_ ? active_device_->value() : std::string("unknown"),
        info.channels, info.sample_rate,
        static_cast<int>(active_config_.format.encoding));
    return result;
}

std::expected<void, AudioError> PlaybackManager::restart() noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        // Fatal 是终态：与 set_playback_device / restart_on_error 一致，拒绝降级回 Inactive。
        log_warn("PlaybackManager: restart rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        // 尚未成功 start 过，没有"旧配置"可重启。
        return std::unexpected(AudioError::NotRunning);
    }
    const auto requested = active_config_.route.endpoint_request();
    log_debug_fmt("PlaybackManager restart: same-device rebuild, route={} device={}",
        audio_route_authority_name(active_config_.route.authority),
        requested ? requested->value() : std::string("system_default"));

    state_.store(PlaybackState::Switching, std::memory_order_release);
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);
    // break-before-make：stop() 同步 join 旧回调线程，返回后旧回调不再
    // 访问 JitterBuffer（AudioPlayback::stop 契约），JB 消费者唯一性在此交接。
    playback_->stop();
    const auto result = start_stream(active_config_, callbacks_);
    if (!result) {
        // A-0 语义：无 fallback 链，失败即停。
        state_.store(PlaybackState::Inactive, std::memory_order_release);
        log_error_fmt("PlaybackManager restart failed: {}",
            audio_error_name(result.error()));
        return result;
    }
    cache_active_device(requested);
    state_.store(PlaybackState::Running, std::memory_order_release);
    log_debug("PlaybackManager restart completed: playback stream rebuilt");
    return result;
}

void PlaybackManager::cache_active_device(
    const std::optional<AudioDeviceId>& requested) noexcept
{
    // 成功 start 后立即落盘「实际输出设备」：优先 stream_info 回读（backend
    // open 后缓存的真实 endpoint/device），回读为空退回请求值。此后
    // previous_active_device 不再依赖 backend 的实时 stream_info 状态——
    // 设备 error 后 stream_info 可能尚未清零或已清零，都不影响切换/回滚决策。
    const auto info = stream_info();
    active_device_ = info.device_id.empty()
        ? requested
        : std::optional<AudioDeviceId> { info.device_id };
}

std::optional<AudioDeviceId> PlaybackManager::previous_active_device() const noexcept
{
    // 优先 PlaybackManager 缓存（生命周期状态），其次 backend 实时回读，
    // 最后退回请求值。三层兜底保证切换/恢复不因 backend 状态未及时更新而丢
    // 失「之前实际在哪个设备上」这一关键信息。
    if (active_device_.has_value()) {
        return active_device_;
    }
    const auto info = stream_info();
    if (!info.device_id.empty()) {
        return info.device_id;
    }
    return active_config_.route.endpoint_request();
}

std::expected<SwitchResult, AudioError> PlaybackManager::switch_to(
    std::optional<AudioDeviceId> target) noexcept
{
    // 事务耗时（诊断）：stop + 候选链 + start 的墙钟总时长。
    const auto switch_started = std::chrono::steady_clock::now();
    // 事务序号：每笔切换递增（成功与否都算），供诊断/UI 判定"又切了一次"。
    // fetch_add 而非 store：本方法只在控制线程串行调用，仍需原子以跨线程读。
    switch_seq_.fetch_add(1, std::memory_order_acq_rel);
    state_.store(PlaybackState::Switching, std::memory_order_release);
    // 撤销对旧流的认领：teardown 期间到达的事件在 start_stream 的过滤里被丢弃。
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);

    // 捕获 previous_active_device（必须在 stop 前回读；stop 后缓存清零）。
    const auto previous = previous_active_device();
    log_info_fmt("PlaybackManager switch begin: target={} previous={} route={}",
        target ? target->value() : std::string("system_default"),
        previous ? previous->value() : std::string("unknown"),
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)));

    // break-before-make：stop() 同步 join 旧回调线程。
    playback_->stop();

    // 候选链（playback_switching_design.md §5）：形态由 target 决定——
    //   显式 target（指定设备 / 用户选择 / Application 意图推导）：完整 fallback
    //     链 [target, previous, system_default]，按 optional<AudioDeviceId>
    //     相等去重（nullopt 与 nullopt 亦相等）。链固定三层，不做全设备遍历；
    //   nullopt target（自动跟随 / 用户跟随）：单候选直达当前系统默认，
    //     不回滚 previous——跟随语义下旧设备正是要离开的，回滚只是延迟失败，
    //     还会引发 tick 的反复横跳；失败即按链耗尽 Fatal，由上层停止或重建。
    std::vector<std::optional<AudioDeviceId>> candidates;
    const auto push_dedup = [&](std::optional<AudioDeviceId> candidate) {
        for (const auto& existing : candidates) {
            if (existing == candidate) {
                return;
            }
        }
        candidates.push_back(std::move(candidate));
    };
    push_dedup(target);
    if (target.has_value()) {
        push_dedup(previous);
        push_dedup(std::nullopt);
    }

    AudioError last_error = AudioError::BackendFailed;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto cfg = active_config_;
        cfg.route = candidate_route(candidates[i]);
        const auto result = start_stream(cfg, callbacks_);
        if (result.has_value()) {
            active_config_ = cfg;
            cache_active_device(candidates[i]);
            // 结果按成功候选的值判定（序号在去重后不可靠）：目标是
            // nullopt 且一次成功 = Switched；落在先前实际设备 = RolledBack；
            // 落系统默认（nullopt 兜底）= FellBackToSystem。
            const auto outcome = i == 0
                ? SwitchOutcome::Switched
                : (candidates[i] ? SwitchOutcome::RolledBack
                                 : SwitchOutcome::FellBackToSystem);
            const SwitchResult switch_result {
                outcome, AudioError::None, switch_duration_ms(switch_started)
            };
            last_switch_result_.store(switch_result, std::memory_order_release);
            state_.store(PlaybackState::Running, std::memory_order_release);
            log_info_fmt(
                "PlaybackManager switch completed: outcome={} device={} (candidates={}) duration_ms={}",
                switch_outcome_name(outcome),
                candidates[i] ? candidates[i]->value() : std::string("system_default"),
                candidates.size(), switch_result.duration_ms);
            return switch_result;
        }
        last_error = result.error();
        // 该候选失败：撤销认领，避免它的迟到事件被误当成当前流。
        active_generation_.store(kNoStreamGeneration, std::memory_order_release);
        log_warn_fmt("PlaybackManager switch candidate {} failed: {}",
            candidates[i] ? candidates[i]->value() : std::string("system_default"),
            audio_error_name(last_error));
    }

    // ---- 链末兜底：重试刚关闭的上一设备（仅瞬时失败）----
    // 见本文件顶部 kSwitchRetry* 的说明：这是"换设备别把整条连接带崩"的
    // 最后一道保险。回到上一设备 = RolledBack（会话继续），回不去才 Fatal。
    //
    // 只对**显式 target** 的事务生效：nullopt（自动跟随 / 用户跟随）的语义是
    // "旧设备正是要离开的那个"，回滚它只会延迟失败并引发横跳（§5），那条路径
    // 保持"单候选直达、失败即 Fatal"。
    if (target.has_value() && previous.has_value()
        && is_transient_switch_error(last_error)) {
        // 这一段最长会阻塞 sum(200ms * n) = 2s，且只有失败才出声；入口先打一行
        // info，否则日志上表现为"switch begin 之后长时间沉默然后突然 Fatal"。
        log_info_fmt(
            "PlaybackManager switch chain exhausted, retrying previous device '{}' "
            "(last_error={} max_attempts={} backoff={}ms*n)",
            previous->value(), audio_error_name(last_error),
            kSwitchRetryAttempts, kSwitchRetryBackoffMs);
        for (unsigned attempt = 1; attempt <= kSwitchRetryAttempts; ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSwitchRetryBackoffMs) * attempt);
            auto cfg = active_config_;
            cfg.route = candidate_route(previous);
            const auto retry = start_stream(cfg, callbacks_);
            if (retry.has_value()) {
                active_config_ = cfg;
                cache_active_device(previous);
                const SwitchResult switch_result { SwitchOutcome::RolledBack,
                    AudioError::None, switch_duration_ms(switch_started) };
                last_switch_result_.store(switch_result, std::memory_order_release);
                state_.store(PlaybackState::Running, std::memory_order_release);
                log_info_fmt(
                    "PlaybackManager switch chain exhausted, recovered by retrying previous device: "
                    "device={} attempt={}/{} waited_ms={} duration_ms={}",
                    previous->value(), attempt, kSwitchRetryAttempts,
                    kSwitchRetryBackoffMs * attempt, switch_result.duration_ms);
                return switch_result;
            }
            last_error = retry.error();
            active_generation_.store(kNoStreamGeneration, std::memory_order_release);
            log_warn_fmt("PlaybackManager switch retry {} on previous device {} failed: {}",
                attempt, previous->value(), audio_error_name(last_error));
            if (!is_transient_switch_error(last_error)) {
                break; // 错误性质变了（如格式不兼容）：重试无意义
            }
        }
    }

    // 链耗尽 = 格式不兼容（或重试超限后进入本路径）：Fatal 终态。
    const SwitchResult switch_result {
        SwitchOutcome::Fatal, last_error, switch_duration_ms(switch_started)
    };
    last_switch_result_.store(switch_result, std::memory_order_release);
    state_.store(PlaybackState::Fatal, std::memory_order_release);
    log_error_fmt("PlaybackManager switch exhausted fallback chain: {}",
        audio_error_name(last_error));
    return std::unexpected(last_error);
}

std::expected<SwitchResult, AudioError> PlaybackManager::set_playback_device(
    std::optional<AudioDeviceId> target) noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        // Fatal 是终态：链耗尽后不再接受事务（supervision 将 stop runtime）。
        log_warn("PlaybackManager: set_playback_device rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }

    // 用户显式选择：两套预算都不计数并重置窗口（防抖策略 §5；内部自动跟随
    // 走 follow_system_default，不经此处）。
    const auto now = std::chrono::steady_clock::now();
    error_restarts_in_window_ = 0;
    window_start_ = now;
    settle_restarts_in_window_ = 0;
    settle_window_start_ = now;
    last_settle_attempt_ = { };

    // sticky 用户意图与归属**立即**更新（含 nullopt = 用户改选"跟随系统"），
    // 不等事务结果：后续 fallback 降级不覆盖它，自动切回与错误驱动 restart
    // 以其为目标。事务失败（链耗尽 -> Fatal）时意图同样保留，便于诊断"用户
    // 想去哪"——这也是把原先分成两处的 preferred_device_ 与 route_mode_ 合并
    // 成单一 RoutePolicy 的收益：不再可能出现"意图已改、归属未改"的中间态。
    //
    // 归属按用户请求推导（不按落点）：显式选设备即 User，即使本次 fallback
    // 降级到系统默认，sticky 与自动切回语义仍然保留；选 nullopt 即 None。
    const auto user_pinned = target.has_value() && !target->empty();
    const auto previous_owner = intent_owner_.load(std::memory_order_acquire);
    set_policy(user_pinned
            ? RoutePolicy::user_pinned(*target, kLossAction)
            : RoutePolicy::follow_system(kLossAction));
    // 用户显式选择是切换事务的**入口**，必须自成一行的 info：紧随其后的
    // "switch begin" 里的 route= 已是新归属，看不出这次是谁发起、归属怎么变的。
    log_info_fmt(
        "PlaybackManager user device selection: target={} owner {} -> {} (both retry budgets reset)",
        user_pinned ? target->value() : std::string("system_default"),
        route_intent_owner_label(previous_owner),
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)));

    return switch_to(std::move(target));
}

bool PlaybackManager::consume_restart_budget() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start_ >= kRetryWindow) {
        error_restarts_in_window_ = 0;
        window_start_ = now;
    }
    if (error_restarts_in_window_ >= kMaxErrorRestarts) {
        const SwitchResult switch_result { SwitchOutcome::Fatal, AudioError::BackendFailed };
        last_switch_result_.store(switch_result, std::memory_order_release);
        state_.store(PlaybackState::Fatal, std::memory_order_release);
        log_error("PlaybackManager: restart retry budget exhausted");
        return false;
    }
    ++error_restarts_in_window_;
    return true;
}

PlaybackManager::SettleBudget PlaybackManager::consume_settle_budget() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (last_settle_attempt_.time_since_epoch().count() != 0
        && now - last_settle_attempt_ < kSettleRetryInterval) {
        // 节流中：**不改任何状态**直接返回。调用方原样返回错误，
        // ClientRuntime 的 supervision tick（500ms）随后经 silent-death 兜底
        // 分支再次驱动——这就是 pacing 的来源，无需在 ioc 线程上 sleep。
        return SettleBudget::Throttled;
    }
    if (now - settle_window_start_ >= kSettleWindow) {
        settle_restarts_in_window_ = 0;
        settle_window_start_ = now;
    }
    if (settle_restarts_in_window_ >= kMaxSettleRestarts) {
        // 不落 Fatal：这里只负责报告"settle 这个假设已经不成立了"，降级决定
        // 由 restart_on_error 做（它同时决定从哪个预算取额度）。仍然记一次
        // 尝试时刻，使降级路径同样受 kSettleRetryInterval 节流——否则三次
        // 降级会背靠背打完，Fatal 来得比日志能读懂的速度还快。
        last_settle_attempt_ = now;
        return SettleBudget::Exhausted;
    }
    last_settle_attempt_ = now;
    ++settle_restarts_in_window_;
    return SettleBudget::Granted;
}

std::expected<SwitchResult, AudioError> PlaybackManager::follow_system_default() noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        log_warn("PlaybackManager: follow_system_default rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!consume_restart_budget()) {
        return std::unexpected(AudioError::BackendFailed);
    }
    log_info_fmt("PlaybackManager auto-follow: target=system_default retry={}/{} in 10s window",
        error_restarts_in_window_, kMaxErrorRestarts);
    // 不改变路由策略与 sticky 意图：调用方已处于跟随系统，intent_ 为空；
    // switch_to 只动 active_config_/active_device_。
    return switch_to(std::nullopt);
}

std::expected<SwitchResult, AudioError> PlaybackManager::restart_on_error() noexcept
{
    if (!playback_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == PlaybackState::Fatal) {
        log_warn("PlaybackManager: restart_on_error rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }

    // ---- 失败分类（§5 rev3）----
    // 「流刚起来就死了」= 平台路由尚未稳定（Android AudioPolicy 在设备转换期
    // 把 DISCONNECTED 投递给当前流），不是设备丢失。两者必须分开计数：混用
    // 一个预算会让一次 250ms 的路由抖动烧光 10s/3 的额度并 Fatal。
    //
    // 三个判据缺一不可：
    //   active_generation_ 有效 —— 当前确实认领着一条流（事务中途它是
    //                             kNoStreamGeneration，不判为 settling）；
    //   !is_running()          —— 那条流**真的死了**。backend 报告致命错误时
    //                             会把 running_ 置 false，这是"死过"的可观测
    //                             事实。只看时间会把"流还活着、上层因别的
    //                             原因要求 restart"误判成路由抖动。
    //   age < kRouteSettleWindow —— 死得太快，快得不像设备被拔掉。
    const auto now = std::chrono::steady_clock::now();
    const auto stream_age = now - stream_started_at_;
    const bool settling
        = active_generation_.load(std::memory_order_acquire) != kNoStreamGeneration
        && !is_running()
        && stream_age < kRouteSettleWindow;

    // settle 预算耗尽后的降级：本次不再重试策略推导的目标，而是直接跟随系统
    // 默认。判据来自实测（temp/android_switch.log）——钉住的设备已拔出时，
    // openStream/requestStart 对它**仍然成功**，DISCONNECTED 在 +17ms 才到；
    // 于是 switch_to(intent) 在候选 0 就返回 Switched，[previous, system_default]
    // 兜底链永不触发，8 次 settle 重试全在原地打转。这说明"路由未稳定"已被
    // 证伪，真实情况是这条路由本身坏了，而 kLossAction = FallbackToSystem
    // 承诺的正是降级，不是 Fatal。
    bool settle_degrade = false;
    if (settling) {
        switch (consume_settle_budget()) {
        case SettleBudget::Granted:
            log_info_fmt(
                "PlaybackManager route-settle restart: stream died {}ms after start, "
                "settle retry={}/{} (device-loss budget untouched)",
                std::chrono::duration_cast<std::chrono::milliseconds>(stream_age).count(),
                settle_restarts_in_window_, kMaxSettleRestarts);
            break;
        case SettleBudget::Throttled:
            // 状态不变，交给 supervision tick（silent-death 兜底分支）再驱动。
            return std::unexpected(AudioError::DeviceDisconnected);
        case SettleBudget::Exhausted:
            settle_degrade = true;
            // 从**设备丢失**预算取额度（而非一次性标志）：否则一条跟随系统默认
            // 的流若也起来就死，降级会无限循环且永不 Fatal。两级预算都耗尽时
            // consume_restart_budget 内部落 Fatal——语义与既有的设备丢失路径一致。
            if (!consume_restart_budget()) {
                log_error_fmt(
                    "PlaybackManager: route settle budget exhausted ({} attempts in {}s) "
                    "and device-loss budget exhausted too; not degrading further",
                    kMaxSettleRestarts,
                    std::chrono::duration_cast<std::chrono::seconds>(kSettleWindow).count());
                return std::unexpected(AudioError::BackendFailed);
            }
            // 日志在取额度**之后**：retry=N/3 是本次降级实际占用的序号，
            // 打在之前会显示 0/3，读日志的人得自己心算偏移。
            log_warn_fmt(
                "PlaybackManager: route settle budget exhausted ({} attempts in {}s), "
                "route is broken rather than settling; degrading to system default "
                "(device-loss budget retry={}/{})",
                kMaxSettleRestarts,
                std::chrono::duration_cast<std::chrono::seconds>(kSettleWindow).count(),
                error_restarts_in_window_, kMaxErrorRestarts);
            break;
        }
    } else if (!consume_restart_budget()) {
        return std::unexpected(AudioError::BackendFailed);
    }

    // 目标由路由策略推导：None -> 系统默认；Application -> 首流钉住的实际
    // 落点；User -> sticky 用户意图（fallback 降级后仍指向用户钉住的设备，
    // 而非当前兜底设备）。settle 降级是唯一例外：它绕开策略直指系统默认，
    // 但**不改策略**——intent_ 保留，钉住的设备回归后仍能自动切回。
    const auto policy = route_policy();
    auto target = settle_degrade
        ? std::optional<AudioDeviceId>(std::nullopt)
        : policy.restart_target(previous_active_device());
    if (!settling) {
        log_info_fmt(
            "PlaybackManager error-driven restart: route={} on_loss={} derived_target={} retry={}/{} in 10s window",
            policy.label(), route_loss_action_name(policy.on_loss),
            target ? target->value() : std::string("system_default"),
            error_restarts_in_window_, kMaxErrorRestarts);
    }

    // 不改变路由策略：fallback 是临时降级，用户意图不动。
    return switch_to(std::move(target));
}

void PlaybackManager::stop() noexcept
{
    log_debug("PlaybackManager stop: tearing down playback stream");
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);
    if (playback_) {
        playback_->stop();
    }
    active_device_.reset();
    state_.store(PlaybackState::Inactive, std::memory_order_release);
}

bool PlaybackManager::tick() noexcept
{
    if (state_.load(std::memory_order_acquire) != PlaybackState::Running) {
        return false; // Switching 事务自身负责路由；Fatal/Inactive 不动作。
    }
    if (device_manager_ == nullptr) {
        return false; // 测试构造无设备入口
    }

    // ---- 第 1 步：设备集合轮询（对所有归属生效）----
    // 这是 Windows 上「钉住设备回归自动切回」的唯一触发源：notify_devices_changed
    // 只有 JNI 调用者，而本函数原先对 owner != None 直接返回——于是钉住的设备
    // 拔掉后能回落（流错误事件驱动），插回来却永远不切回。
    // 去抖在 DeviceSetPoller 内（连续两次一致才上报）：WASAPI 在设备转换期可能
    // 瞬时枚举不全，单次采样就动手会 restart 一条健康的流，三次就烧光 10s/3
    // 预算落 Fatal。Android 的 enumerate 只给合成空 id，poll() 恒返回 nullopt，
    // 决策仍由推送驱动；两条路径共用 on_devices_changed，互不干扰。
    if (device_poller_) {
        if (const auto change = device_poller_->poll()) {
            // 只打**增量**，且基线（added/removed 皆空）不打：present 全集是
            // 五六条 endpoint GUID，每 500ms 刷一遍会把 debug 日志淹掉，而
            // debug 主要用来看 diagnostics。检测到了但决策为不动作时，这行是
            // 唯一的证据（不会有后续 info 切换行）。
            if (!change->added.empty() || !change->removed.empty()) {
                log_debug_fmt("PlaybackManager device poll: added={} removed={}",
                    format_device_ids(change->added),
                    format_device_ids(change->removed));
            }
            if (on_devices_changed(change->present)) {
                return true;
            }
        }
    }

    // ---- 第 2 步：仅跟随系统时轮询系统默认输出设备变化 ----
    // 其它归属下用户/App 意图优先，不查询也不跟随（查询成本只留给需要它的归属）。
    // 与第 1 步互补：第 1 步只在设备**集合**变化时跟随，本步覆盖"集合没变但
    // 用户改了默认设备"（如在系统设置里切换默认输出）。
    if (intent_owner_.load(std::memory_order_acquire) != RouteIntentOwner::None) {
        return false;
    }
    const auto current = device_manager_->default_device(AudioDeviceDirection::OUTPUT);
    if (!current || current->id.empty()) {
        // 无默认设备信息（如 Android 的合成空 id）：该平台由上层路由检测驱动。
        return false;
    }
    if (!active_device_.has_value()) {
        // 当前实际设备未知（backend 未回读 device_id）：无法比较，跳过，
        // 避免每次 tick 都误判「已变化」造成自持的重路由循环。
        return false;
    }
    if (*active_device_ == current->id) {
        return false; // 默认设备未变化
    }
    log_info_fmt(
        "PlaybackManager: system default output changed from '{}' to '{}', following",
        active_device_->value(), current->id.value());
    // 重路由到新默认（nullopt = 跟随系统）：内部跟随走完整预算约束，
    // 不重置重试窗口（只属于用户显式选择）。
    const auto result = follow_system_default();
    return result.has_value()
        && state_.load(std::memory_order_acquire) == PlaybackState::Running;
}

bool PlaybackManager::on_devices_changed(const std::vector<AudioDeviceId>& present) noexcept
{
    if (!playback_ || !callbacks_) {
        return false;
    }
    // 每份连接的首份快照只作基线：初始设备列表不是"新增设备"。
    if (!known_devices_valid_) {
        known_devices_ = present;
        known_devices_valid_ = true;
        // 每条连接只出现一次（start() 清基线）：它是"设备快照推送通道是否活着"
        // 的唯一证据。Windows 无人推送（notify_devices_changed 目前只有 JNI 调用），
        // 因此这行缺席本身就说明该平台的钉住回归只能靠流错误事件驱动。
        log_info_fmt("PlaybackManager: device event baseline recorded ({} devices)",
            present.size());
        return false;
    }
    if (state_.load(std::memory_order_acquire) != PlaybackState::Running) {
        // 非运行期（Switching 事务进行中 / Fatal）：快照照收，事务自身
        // 负责路由；下一份事件以新基线重新决策。
        known_devices_ = present;
        return false;
    }

    const auto owner = intent_owner_.load(std::memory_order_acquire);
    const auto active = active_device_;
    const bool active_known = active.has_value() && !active->value().empty();
    const bool active_gone = active_known && !std::ranges::contains(present, *active);
    // 是否跑了事务以 switch_seq 判定（switch_to 入口无条件 fetch_add）：
    // 节流/预算耗尽未进 switch_to 时 seq 不变 → 返回 false，service 不清错误；
    // 真切换（含降级、含链耗尽 Fatal）seq 必变 → true。手写 acted 会把
    // “被节流的空操作”也报成 acted，导致真错误被清零（假“已恢复”）。
    const auto seq_before = switch_seq_.load(std::memory_order_acquire);

    if (active_gone) {
        // 当前输出设备已消失：提前 restart（流的错误事件通常随后到达；
        // Switching 期间事件不派发，且 ClientRuntime 会吸收错误标志，
        // 不会双重 restart）。restart_on_error 路径：策略推导目标 +
        // fallback 链 + 重试预算，保留路由策略。
        // auto_return 直接回答"这台设备插回来还会不会切回去"：只有 User 归属
        // 是 sticky 的（RoutePolicy::auto_returns）。Application 只表达"别乱跑"，
        // None 根本没有意图可回。少了这个字段，读日志的人得先从 route= 标签
        // 反查归属语义才能判断后续行为。
        log_info_fmt(
            "PlaybackManager: active device '{}' no longer present, eager restart "
            "(route={} auto_return={})",
            active->value(), route_intent_owner_label(owner),
            route_policy().auto_returns() ? "yes" : "no");
        (void)restart_on_error();
    } else if (owner == RouteIntentOwner::None) {
        // 跟随系统：新增可切换设备 → 重开流跟随。默认可查询的平台先确认
        // 默认真变了再动手（无关设备到达不值得一次 stop/start；tick 会兜底
        // 真变化）；查不到默认的平台（Android 合成空 id）按到达即跟随。
        bool has_new = false;
        for (const auto& id : present) {
            if (!std::ranges::contains(known_devices_, id)) {
                has_new = true;
                break;
            }
        }
        if (has_new) {
            bool default_changed = true;
            if (device_manager_ != nullptr) {
                const auto current = device_manager_->default_device(AudioDeviceDirection::OUTPUT);
                if (current && !current->id.empty() && active_known && *active == current->id) {
                    default_changed = false;
                }
            }
            if (default_changed) {
                log_info("PlaybackManager: new output device appeared, following system default");
                (void)follow_system_default();
            }
        }
    } else if (owner == RouteIntentOwner::User
        && intent_.has_value()
        && !(active_known && *active == *intent_)
        && std::ranges::contains(present, *intent_)) {
        // 用户钉住的设备回归（当前因 fallback 在别的设备上）：自动切回。
        // Application 归属不自动切回——它只表达"别乱跑"，不是"我认准了这个"。
        // 自动事务同样消费重试预算（防设备反复出现/消失的风暴；耗尽即
        // Fatal，与错误驱动/跟随同规则）；失败回滚后 intent_ 仍保留，
        // 下次回归可重试。
        log_info_fmt("PlaybackManager: pinned device '{}' re-appeared, switching back",
            intent_->value());
        if (consume_restart_budget()) {
            (void)switch_to(*intent_);
        }
        // 预算耗尽时不再尝试（已落 Fatal），但事件已消费：落到底部的基线更新。
    }
    // Application 归属：钉住实际设备，设备集合变化不驱动任何动作（活跃设备
    // 消失由上方 active_gone 分支统一处理）。

    known_devices_ = present;
    // seq 变了 == 真跑了一笔事务；Fatal（预算耗尽，无事务但终态）同样算消费，
    // service 靠 Running 守卫不会误清；唯独节流（Running + 无事务）报 false。
    return switch_seq_.load(std::memory_order_acquire) != seq_before
        || state_.load(std::memory_order_acquire) == PlaybackState::Fatal;
}

} // namespace aqua::audio
