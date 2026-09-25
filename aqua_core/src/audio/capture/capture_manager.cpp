#include "aqua/audio/capture/capture_manager.h"

#include "aqua/audio/audio_switch_result.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace aqua::audio {

namespace {

    // 候选 -> 路由请求：有 id 即绑定，nullopt 即交还路由权给平台。
    [[nodiscard]] AudioRoute candidate_route(
        const std::optional<AudioDeviceId>& candidate)
    {
        return candidate ? AudioRoute::pin(*candidate) : AudioRoute::follow_system();
    }

    [[nodiscard]] AudioDeviceDirection route_direction(AudioCaptureSource source) noexcept
    {
        switch (source) {
        case AudioCaptureSource::INPUT_DEVICE:
            return AudioDeviceDirection::INPUT;
        case AudioCaptureSource::OUTPUT_LOOPBACK:
            return AudioDeviceDirection::OUTPUT;
        }
        return AudioDeviceDirection::NONE;
    }

} // namespace

CaptureManager::CaptureManager(AudioDeviceManager& device_manager)
    : capture_(create_capture(device_manager))
    , device_manager_(&device_manager)
{
    if (!capture_) {
        log_error("CaptureManager: audio capture backend is unavailable on this platform");
    }
}

CaptureManager::CaptureManager(std::unique_ptr<AudioCapture> capture,
    AudioDeviceManager* device_manager)
    : capture_(std::move(capture))
    , device_manager_(device_manager)
{
}

void CaptureManager::set_policy(RoutePolicy policy) noexcept
{
    intent_ = std::move(policy.intent);
    // 诊断投影与 sticky 意图在同一次写入内保持一致；跨线程读者只看到 owner。
    intent_owner_.store(policy.owner, std::memory_order_release);
}

AudioCaptureInfo CaptureManager::info() const noexcept
{
    return capture_ != nullptr ? capture_->info() : AudioCaptureInfo { };
}

std::expected<void, AudioError> CaptureManager::start_stream(
    const AudioCaptureConfig& route_config,
    const std::shared_ptr<CallbackBundle>& bundle,
    std::optional<AudioDeviceId>& resolved_device) noexcept
{
    resolved_device.reset();
    AudioCaptureConfig start_config = route_config;
    const auto requested = route_config.route.endpoint_request();

    // 候选解析：把路由请求解析成具体设备 id（跟随系统 -> 当前系统默认）。
    // active_device_ 以解析结果为准（capture 无设备回读），tick() 的默认跟随
    // 比较与 previous_active_device 都依赖这个身份。
    // 解析失败 = 该候选不可用（如系统默认设备不存在 -> DeviceNotFound）。
    if (device_manager_ != nullptr) {
        const auto direction = route_direction(route_config.source);
        const auto resolved = device_manager_->resolve(direction, requested);
        if (!resolved) {
            log_warn_fmt("CaptureManager: candidate resolve failed (device={}): {}",
                requested ? requested->value() : std::string("system_default"),
                audio_error_name(resolved.error()));
            return std::unexpected(resolved.error());
        }
        // 刻意把解析结果钉进交给 backend 的请求：capture 没有设备回读，只有
        // 这样 active_device_ 才与实际流严格一致（tick 的默认变化比较依赖它）。
        // 「跟随系统默认」由 tick() 检测到默认变化后重开来实现，不依赖流层面的
        // 自动重路由——因此钉住解析值不会削弱跟随语义。
        start_config.route = AudioRoute::pin(resolved->id);
        resolved_device = resolved->id;
    } else {
        // 测试构造无设备系统入口：直接使用请求值。
        resolved_device = requested;
    }

    // 包装转发：lambda 持有 bundle 的 shared_ptr 引用（AudioCaptureCallback
    // 不可拷贝；包装后 restart 可重复传入同一回调）。
    auto wrapped_block = [bundle](const AudioBlock& block) noexcept {
        bundle->block(block);
    };
    AudioCaptureEventCallback wrapped_event;
    if (bundle->event) {
        // provenance 契约的 manager 侧落点：只放行当前 generation 的事件。
        // 事务开始时 active_generation_ 已被置为 kNoStreamGeneration，因此
        // teardown 期间 backend 投递的旧流临终错误在此即被丢弃。
        wrapped_event = [bundle, this](const AudioStreamEvent& event) noexcept {
            if (event.generation != active_generation_.load(std::memory_order_acquire)) {
                stale_events_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            bundle->event(event);
        };
    }
    const auto result = capture_->start(start_config,
        std::move(wrapped_block), std::move(wrapped_event));
    if (!result) {
        return result;
    }
    // 认领这条流：此后只有它的事件能穿过上面的过滤。
    active_generation_.store(capture_->generation(), std::memory_order_release);

    // 格式不可变（共享原则）：会话格式在首流钉进 active_config，此处对
    // 每个成功候选复核实际流格式（belt-and-braces——显式请求的格式
    // WASAPI 已校验；backend 未严格履约时在此兜底，该候选按
    // FormatUnsupported 处理）。
    if (route_config.format && info().format != *route_config.format) {
        log_warn_fmt("CaptureManager: backend started with format {}ch/{}Hz/enc={} but session requires {}ch/{}Hz/enc={}",
            info().format.channels, info().format.sample_rate,
            static_cast<int>(info().format.encoding),
            route_config.format->channels, route_config.format->sample_rate,
            static_cast<int>(route_config.format->encoding));
        active_generation_.store(kNoStreamGeneration, std::memory_order_release);
        capture_->stop();
        return std::unexpected(AudioError::FormatUnsupported);
    }
    return { };
}

std::expected<void, AudioError> CaptureManager::start(
    const AudioCaptureConfig& config,
    AudioCaptureCallback block_callback,
    AudioCaptureEventCallback event_callback) noexcept
{
    if (!capture_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!block_callback) {
        // 后端只见到非空的包装回调，空回调校验收敛在 manager。
        return std::unexpected(AudioError::InvalidArgument);
    }

    auto bundle = std::make_shared<CallbackBundle>();
    bundle->block = std::move(block_callback);
    bundle->event = std::move(event_callback);

    state_.store(CaptureSwitchState::Starting, std::memory_order_release);
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);
    std::optional<AudioDeviceId> resolved_device;
    const auto result = start_stream(config, bundle, resolved_device);
    if (!result) {
        state_.store(CaptureSwitchState::Inactive, std::memory_order_release);
        return result;
    }

    active_config_ = config;
    // 会话格式钉死（Format immutable）：首流实际格式成为后续所有 restart
    // 候选的显式请求格式；候选设备不原生支持即 FormatUnsupported。
    active_config_.format = info().format;
    // 设备集合轮询按本会话的 source 方向建（playback 侧方向恒为 OUTPUT，
    // 在构造期即建）。方向未知或无设备入口时不建，tick 第 1 步整体跳过。
    const auto poll_direction = route_direction(config.source);
    if (device_manager_ != nullptr && poll_direction != AudioDeviceDirection::NONE) {
        device_poller_.emplace(*device_manager_, poll_direction);
    } else {
        device_poller_.reset();
    }
    callbacks_ = std::move(bundle);
    active_device_ = resolved_device;
    // 路由策略由请求推导：pin -> User（sticky = CLI 配置值）；跟随系统 -> None。
    // server 无 UI，不存在 Application（"保持当前实际设备"）归属。
    set_policy(RoutePolicy::from_route(config.route, kLossAction));
    // sticky 用户意图与重试预算随新会话重置。
    auto_restarts_in_window_ = 0;
    window_start_ = std::chrono::steady_clock::now();
    last_switch_result_.store(SwitchResult { }, std::memory_order_release);
    state_.store(CaptureSwitchState::Running, std::memory_order_release);
    log_info_fmt("CaptureManager started: route={} on_loss={} device={} format={}ch/{}Hz/enc={}",
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)),
        route_loss_action_name(kLossAction),
        active_device_ ? active_device_->value() : std::string("unknown"),
        info().format.channels, info().format.sample_rate,
        static_cast<int>(info().format.encoding));
    return { };
}

std::optional<AudioDeviceId> CaptureManager::previous_active_device() const noexcept
{
    // 优先成功 start 时落盘的实际设备（候选解析结果），其次退回请求值。
    if (active_device_.has_value()) {
        return active_device_;
    }
    return active_config_.route.endpoint_request();
}

bool CaptureManager::consume_restart_budget() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start_ >= kRetryWindow) {
        auto_restarts_in_window_ = 0;
        window_start_ = now;
    }
    if (auto_restarts_in_window_ >= kMaxAutoRestarts) {
        return false;
    }
    ++auto_restarts_in_window_;
    return true;
}

std::expected<SwitchResult, AudioError> CaptureManager::switch_to(
    std::optional<AudioDeviceId> target) noexcept
{
    // 事务耗时（诊断）：stop + 候选链 + start 的墙钟总时长。
    const auto switch_started = std::chrono::steady_clock::now();
    state_.store(CaptureSwitchState::Switching, std::memory_order_release);
    // 撤销对旧流的认领：teardown 期间到达的事件在 start_stream 的过滤里被丢弃。
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);

    // 捕获 previous_active_device（必须在 stop 前读取）。
    const auto previous = previous_active_device();
    log_info_fmt("CaptureManager switch begin: target={} previous={} route={}",
        target ? target->value() : std::string("system_default"),
        previous ? previous->value() : std::string("unknown"),
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)));

    // break-before-make：stop() 同步 join 音频线程，返回后旧回调不再
    // 访问 packetizer（AudioCapture::stop 契约），生产者唯一性在此交接。
    // packetizer / network / session 全程不动——时间线不变式（§8）。
    capture_->stop();

    // 生产者空档：此刻没有任何采集生产者。通知 runtime 清理属于旧生产者的
    // 残留状态（packetizer 的半个 pending 帧），避免新设备的数据把它补齐。
    // switch_to() 是 noexcept 而 hook 是可抛的 std::function：兜住，否则一次
    // 抛异常（发生在旧流已拆、新流未起之间）直接 terminate 整个 server 进程。
    if (producer_gap_hook_) {
        try {
            producer_gap_hook_();
        } catch (const std::exception& e) {
            log_error_fmt("CaptureManager producer-gap hook threw: {}",
                format_exception_message(e));
        } catch (...) {
            log_error("CaptureManager producer-gap hook threw unknown exception");
        }
    }

    // 候选链（capture_switching_design.md §5）：候选保持“未解析”形态，
    // 每次尝试由 start_stream 按当前 source 方向（input->INPUT /
    // loopback->OUTPUT）重新 resolve——nullopt 永远指向尝试那一刻的
    // 系统默认，跟随语义不因事务中途的默认变化而失效。禁止提前解析
    // 去重：事务中途默认消失/变化时，显式 previous 候选是救命回退，
    // 提前拍快照去重会把它误删成 Fatal。
    //   跟随系统 -> [target(nullopt), previous]：跟随系统默认，
    //                 失败回滚 previous，再兜底系统默认（nullopt）；
    //   钉住意图 -> [target]：显式 --capture-device-id 钉住该设备，不可用即
    //                 Fatal -> stop，绝不降级到系统默认（"只要这个设备
    //                 的数据"语义；与 client 侧"永不主动静音"的移动端
    //                 取舍不同，见 RouteLossAction）。
    // 显式 target 落到 nullopt 兜底（FellBackToSystem）是直接调用
    // switch_to 的应急语义，内部路径（restart/tick 均传 nullopt）走不到，
    // 分支为未来手动切换入口保留。
    // 按 optional<AudioDeviceId> 相等去重（未解析值比较）。
    std::vector<std::optional<AudioDeviceId>> candidates;
    const auto push_dedup = [&](std::optional<AudioDeviceId> candidate) {
        for (const auto& existing : candidates) {
            if (existing == candidate) {
                return;
            }
        }
        candidates.push_back(std::move(candidate));
    };
    const bool pinned = intent_owner_.load(std::memory_order_acquire)
        != RouteIntentOwner::None;
    push_dedup(target);
    if (!pinned) {
        push_dedup(previous);
        push_dedup(std::nullopt);
    }

    AudioError last_error = AudioError::BackendFailed;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto cfg = active_config_; // source + 钉死的会话 format + buffer 参数
        cfg.route = candidate_route(candidates[i]);
        std::optional<AudioDeviceId> resolved_device;
        const auto result = start_stream(cfg, callbacks_, resolved_device);
        if (result.has_value()) {
            active_config_ = cfg;
            active_device_ = resolved_device;
            // 结果按成功候选的角色判定（序号在去重后不可靠）：首候选一次
            // 成功 = Switched；落在先前实际设备 = RolledBack；显式候选全
            // 败后落系统默认（nullopt 兜底）= FellBackToSystem。
            const auto outcome = i == 0
                ? SwitchOutcome::Switched
                : (candidates[i] ? SwitchOutcome::RolledBack
                                 : SwitchOutcome::FellBackToSystem);
            const SwitchResult switch_result { outcome, AudioError::None, switch_duration_ms(switch_started) };
            last_switch_result_.store(switch_result, std::memory_order_release);
            state_.store(CaptureSwitchState::Running, std::memory_order_release);
            log_info_fmt(
                "CaptureManager switch completed: outcome={} device={} (candidates={}) duration_ms={}",
                switch_outcome_name(outcome),
                active_device_ ? active_device_->value() : std::string("unknown"),
                candidates.size(), switch_result.duration_ms);
            return switch_result;
        }
        last_error = result.error();
        // 该候选失败：撤销认领，避免它的迟到事件被误当成当前流。
        active_generation_.store(kNoStreamGeneration, std::memory_order_release);
        log_warn_fmt("CaptureManager switch candidate {} failed: {}",
            candidates[i] ? candidates[i]->value() : std::string("system_default"),
            audio_error_name(last_error));
    }

    // 链耗尽 = 格式不兼容（或重试超限后进入本路径）：Fatal 终态，
    // 决策者（CLI control timer）据此终止会话（§5 Fatal 语义：无
    // capture 的 server 会话无意义）。
    const SwitchResult switch_result {
        SwitchOutcome::Fatal, last_error, switch_duration_ms(switch_started)
    };
    last_switch_result_.store(switch_result, std::memory_order_release);
    state_.store(CaptureSwitchState::Fatal, std::memory_order_release);
    log_error_fmt("CaptureManager switch exhausted fallback chain: {}",
        audio_error_name(last_error));
    return std::unexpected(last_error);
}

std::expected<SwitchResult, AudioError> CaptureManager::restart_on_error() noexcept
{
    if (!capture_) {
        return std::unexpected(AudioError::BackendFailed);
    }
    if (!callbacks_) {
        return std::unexpected(AudioError::NotRunning);
    }
    if (state_.load(std::memory_order_acquire) == CaptureSwitchState::Fatal) {
        // Fatal 是终态：链耗尽后不再接受事务。
        log_warn("CaptureManager: restart_on_error rejected in Fatal state");
        return std::unexpected(AudioError::BackendFailed);
    }

    // 重试上限（§5）：所有自动 restart 共享 10s/3 预算，超限按链耗尽
    // 处理（防设备反复插拔风暴；server 无手动切换，无窗口重置来源）。
    if (!consume_restart_budget()) {
        const SwitchResult switch_result { SwitchOutcome::Fatal, AudioError::BackendFailed };
        last_switch_result_.store(switch_result, std::memory_order_release);
        state_.store(CaptureSwitchState::Fatal, std::memory_order_release);
        log_error("CaptureManager: auto-restart retry budget exhausted (10s/3)");
        return std::unexpected(AudioError::BackendFailed);
    }

    // 目标由路由策略推导（§4）：None -> 系统默认；User -> sticky 配置设备
    // （始终指向用户钉住的设备，不因任何降级而改变——server 侧本就不降级）。
    const auto policy = route_policy();
    auto target = policy.restart_target(active_device_);
    log_info_fmt("CaptureManager error-driven restart: route={} on_loss={} derived_target={} retry={}/{} in 10s window",
        policy.label(), route_loss_action_name(policy.on_loss),
        target ? target->value() : std::string("system_default"),
        auto_restarts_in_window_, kMaxAutoRestarts);

    // 不改变路由策略：跟随系统的 fallback 是临时降级，用户意图不动。
    return switch_to(std::move(target));
}

bool CaptureManager::tick() noexcept
{
    if (state_.load(std::memory_order_acquire) != CaptureSwitchState::Running) {
        return false; // Switching 事务自身负责路由；Fatal/Inactive 不动作。
    }
    if (device_manager_ == nullptr || !callbacks_) {
        return false; // 测试构造无设备入口 / 未运行
    }
    const auto direction = route_direction(active_config_.source);
    if (direction == AudioDeviceDirection::NONE) {
        return false;
    }

    // ---- 第 1 步：设备集合轮询（对所有归属生效）----
    // 去抖在 DeviceSetPoller 内（连续两次一致才上报）：单次采样不可信，设备
    // 转换期枚举可能瞬时不全，误判会 restart 一条健康的采集流。
    // Android 的 enumerate 只给合成空 id，poll() 恒返回 nullopt（capture 目前
    // 也只在 Windows 落地）。
    if (device_poller_) {
        if (const auto change = device_poller_->poll()) {
            // 只打**增量**，基线不打（理由同 PlaybackManager::tick）。
            if (!change->added.empty() || !change->removed.empty()) {
                log_debug_fmt("CaptureManager device poll: added={} removed={}",
                    format_device_ids(change->added),
                    format_device_ids(change->removed));
            }
            if (on_device_set_changed(change->present)) {
                return true;
            }
        }
    }

    // ---- 第 2 步：仅跟随系统时轮询系统默认设备变化 ----
    // 钉住意图下用户意图优先，不查询也不跟随（查询成本只留给需要的归属）。
    // 与第 1 步互补：第 1 步只在设备**集合**变化时动作，本步覆盖"集合没变但
    // 用户改了默认设备"。
    if (intent_owner_.load(std::memory_order_acquire) != RouteIntentOwner::None) {
        return false;
    }
    const auto current = device_manager_->default_device(direction);
    if (!current || current->id.empty()) {
        return false; // 无默认设备信息：无法比较，跳过。
    }
    if (!active_device_.has_value()) {
        // 当前实际设备未知：无法比较，跳过，避免每次 tick 都误判
        // 「已变化」造成自持的重路由循环。
        return false;
    }
    if (*active_device_ == current->id) {
        return false; // 默认设备未变化
    }
    log_info_fmt(
        "CaptureManager: system default device changed from '{}' to '{}', following",
        active_device_->value(), current->id.value());
    // 默认变化驱动的自动 restart（§6 路径 2）：与错误驱动共享重试
    // 预算（§5），目标 nullopt = 跟随新默认；不改变路由策略。
    if (!consume_restart_budget()) {
        const SwitchResult switch_result { SwitchOutcome::Fatal, AudioError::BackendFailed };
        last_switch_result_.store(switch_result, std::memory_order_release);
        state_.store(CaptureSwitchState::Fatal, std::memory_order_release);
        log_error("CaptureManager: auto-restart retry budget exhausted (10s/3) on default-device follow");
        return false; // 未执行事务（预算拒绝）；决策者按 Fatal 终止
    }
    (void)switch_to(std::nullopt);
    return true; // 已执行跟随切换事务（结果看 state / last_switch_result）
}

bool CaptureManager::on_device_set_changed(
    const std::vector<AudioDeviceId>& present) noexcept
{
    if (!active_device_ || active_device_->empty()) {
        // 当前实际设备未知（backend 未回读 / 候选未解析）：无从判断是否消失。
        return false;
    }
    if (std::ranges::contains(present, *active_device_)) {
        // 正在采集的设备仍在：插拔的是别的设备。跟随系统的默认变化由 tick
        // 第 2 步覆盖；钉住意图下别的设备来来去去与本会话无关。
        return false;
    }
    // 正在采集的设备已消失。走 restart_on_error 而不是直接 Fatal：由
    // RoutePolicy 决定后果——钉住归属下候选链只有 [intent]，解析必然失败 ->
    // 链耗尽 -> Fatal -> server 退出（"只要这个设备的数据"）；跟随系统下
    // 重开当前默认。两条都已是既有语义，此处只是把发现时机从"等 backend
    // 投递流错误"提前到轮询，端点挂死而不报错时也能退出。
    // on_loss 直接回答后果：capture 恒为 fatal，钉住归属下这一行之后就是一路
    // 链耗尽 -> Fatal -> server 退出，绝不降级到别的采集源。
    log_info_fmt(
        "CaptureManager: active device '{}' no longer present, eager restart "
        "(route={} on_loss={})",
        active_device_->value(),
        route_intent_owner_label(intent_owner_.load(std::memory_order_acquire)),
        route_loss_action_name(kLossAction));
    (void)restart_on_error();
    return true;
}

void CaptureManager::set_producer_gap_hook(std::function<void()> hook) noexcept
{
    producer_gap_hook_ = std::move(hook);
}

void CaptureManager::stop() noexcept
{
    log_debug("CaptureManager stop: tearing down capture stream");
    active_generation_.store(kNoStreamGeneration, std::memory_order_release);
    if (capture_) {
        capture_->stop();
    }
    active_device_.reset();
    state_.store(CaptureSwitchState::Inactive, std::memory_order_release);
}

} // namespace aqua::audio
