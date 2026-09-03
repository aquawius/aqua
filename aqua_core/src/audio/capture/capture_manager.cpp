#include "aqua/audio/capture/capture_manager.h"

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/logger/logger.h"

#include <vector>

namespace aqua::audio {

namespace {

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

const AudioCaptureInfo& CaptureManager::info() const noexcept
{
    static const AudioCaptureInfo kEmpty { };
    return capture_ != nullptr ? capture_->info() : kEmpty;
}

std::expected<void, AudioError> CaptureManager::start_stream(
    const AudioCaptureConfig& route_config,
    const std::shared_ptr<CallbackBundle>& bundle,
    std::optional<AudioDeviceId>& resolved_device) noexcept
{
    resolved_device.reset();
    AudioCaptureConfig start_config = route_config;

    // 候选解析：把 optional 请求解析成具体设备 id（nullopt -> 当前系统
    // 默认）。active_device_ 以解析结果为准（capture 无设备回读），
    // tick() 的默认跟随比较与 previous_active_device 都依赖这个身份。
    // 解析失败 = 该候选不可用（如系统默认设备不存在 -> DeviceNotFound）。
    if (device_manager_ != nullptr) {
        const auto direction = route_direction(route_config.source);
        const auto resolved = device_manager_->resolve(direction, route_config.device);
        if (!resolved) {
            log_warn_fmt("CaptureManager: candidate resolve failed (device={}): {}",
                route_config.device ? route_config.device->value() : std::string("system_default"),
                audio_error_name(resolved.error()));
            return std::unexpected(resolved.error());
        }
        start_config.device = resolved->id;
        resolved_device = resolved->id;
    } else {
        // 测试构造无设备系统入口：直接使用请求值。
        resolved_device = route_config.device;
    }

    // 包装转发：lambda 持有 bundle 的 shared_ptr 引用（AudioCaptureCallback
    // 不可拷贝；包装后 restart 可重复传入同一回调）。
    auto wrapped_block = [bundle](const AudioBlock& block) noexcept {
        bundle->block(block);
    };
    AudioCaptureEventCallback wrapped_event;
    if (bundle->event) {
        wrapped_event = [bundle](AudioError error) noexcept {
            bundle->event(error);
        };
    }
    const auto result = capture_->start(start_config,
        std::move(wrapped_block), std::move(wrapped_event));
    if (!result) {
        return result;
    }

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
    callbacks_ = std::move(bundle);
    active_device_ = resolved_device;
    // sticky 用户意图与重试预算随新会话重置。
    preferred_device_ = config.device;
    auto_restarts_in_window_ = 0;
    window_start_ = std::chrono::steady_clock::now();
    route_mode_.store(config.device ? CaptureRouteMode::PreferredDevice
                                    : CaptureRouteMode::FollowSystem,
        std::memory_order_release);
    last_switch_result_.store(SwitchResult { }, std::memory_order_release);
    state_.store(CaptureSwitchState::Running, std::memory_order_release);
    log_info_fmt("CaptureManager started: route={} device={} format={}ch/{}Hz/enc={}",
        capture_route_mode_name(route_mode_.load(std::memory_order_acquire)),
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
    return active_config_.device;
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
    state_.store(CaptureSwitchState::Switching, std::memory_order_release);

    // 捕获 previous_active_device（必须在 stop 前读取）。
    const auto previous = previous_active_device();
    log_info_fmt("CaptureManager switch begin: target={} previous={} route_mode={}",
        target ? target->value() : std::string("system_default"),
        previous ? previous->value() : std::string("unknown"),
        capture_route_mode_name(route_mode_.load(std::memory_order_acquire)));

    // break-before-make：stop() 同步 join 音频线程，返回后旧回调不再
    // 访问 packetizer（AudioCapture::stop 契约），生产者唯一性在此交接。
    // packetizer / network / session 全程不动——时间线不变式（§8）。
    capture_->stop();

    // 候选链（capture_switching_design.md §5）：[target, previous,
    // system_default]，按 optional<AudioDeviceId> 相等去重。链固定三层，
    // 不做全设备遍历。
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
    push_dedup(previous);
    push_dedup(std::nullopt);

    AudioError last_error = AudioError::BackendFailed;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto cfg = active_config_; // source + 钉死的会话 format + buffer 参数
        cfg.device = candidates[i];
        std::optional<AudioDeviceId> resolved_device;
        const auto result = start_stream(cfg, callbacks_, resolved_device);
        if (result.has_value()) {
            active_config_ = cfg;
            active_device_ = resolved_device;
            // 结果按成功候选的值判定（序号在去重后不可靠）：目标是
            // nullopt 且一次成功 = Switched；落在先前实际设备 = RolledBack；
            // 落系统默认（nullopt 兜底）= FellBackToSystem。
            const auto outcome = i == 0
                ? SwitchOutcome::Switched
                : (candidates[i] ? SwitchOutcome::RolledBack
                                 : SwitchOutcome::FellBackToSystem);
            const SwitchResult switch_result { outcome, AudioError::None };
            last_switch_result_.store(switch_result, std::memory_order_release);
            state_.store(CaptureSwitchState::Running, std::memory_order_release);
            log_info_fmt(
                "CaptureManager switch completed: outcome={} device={} (candidates={})",
                switch_outcome_name(outcome),
                active_device_ ? active_device_->value() : std::string("unknown"),
                candidates.size());
            return switch_result;
        }
        last_error = result.error();
        log_warn_fmt("CaptureManager switch candidate {} failed: {}",
            candidates[i] ? candidates[i]->value() : std::string("system_default"),
            audio_error_name(last_error));
    }

    // 链耗尽 = 格式不兼容（或重试超限后进入本路径）：Fatal 终态，
    // 决策者（CLI control timer）据此终止会话（§5 Fatal 语义：无
    // capture 的 server 会话无意义）。
    const SwitchResult switch_result { SwitchOutcome::Fatal, last_error };
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

    // 目标由路由模式推导（§4）：FollowSystem -> 系统默认；
    // PreferredDevice -> sticky 配置设备（fallback 降级后仍指向用户
    // 钉住的设备，而非当前兜底设备）。
    std::optional<AudioDeviceId> target;
    const auto mode = route_mode_.load(std::memory_order_acquire);
    switch (mode) {
    case CaptureRouteMode::FollowSystem:
        target = std::nullopt;
        break;
    case CaptureRouteMode::PreferredDevice:
        target = preferred_device_;
        break;
    }
    log_info_fmt("CaptureManager error-driven restart: route_mode={} derived_target={} retry={}/{} in 10s window",
        capture_route_mode_name(mode),
        target ? target->value() : std::string("system_default"),
        auto_restarts_in_window_, kMaxAutoRestarts);

    // 不改变路由模式：fallback 是临时降级，用户意图不动。
    return switch_to(std::move(target));
}

void CaptureManager::tick() noexcept
{
    // 仅 FollowSystem 模式轮询系统默认设备变化；PreferredDevice 用户意图
    // 优先，不查询也不跟随（查询成本只留给需要它的模式）。
    if (route_mode_.load(std::memory_order_acquire) != CaptureRouteMode::FollowSystem) {
        return;
    }
    if (state_.load(std::memory_order_acquire) != CaptureSwitchState::Running) {
        return; // Switching 事务自身负责路由；Fatal/Inactive 不动作。
    }
    if (device_manager_ == nullptr || !callbacks_) {
        return; // 测试构造无设备入口 / 未运行
    }
    const auto direction = route_direction(active_config_.source);
    if (direction == AudioDeviceDirection::NONE) {
        return;
    }
    const auto current = device_manager_->default_device(direction);
    if (!current || current->id.empty()) {
        return; // 无默认设备信息：无法比较，跳过。
    }
    if (!active_device_.has_value()) {
        // 当前实际设备未知：无法比较，跳过，避免每次 tick 都误判
        // 「已变化」造成自持的重路由循环。
        return;
    }
    if (*active_device_ == current->id) {
        return; // 默认设备未变化
    }
    log_info_fmt(
        "CaptureManager: system default device changed from '{}' to '{}', following",
        active_device_->value(), current->id.value());
    // 默认变化驱动的自动 restart（§6 路径 2）：与错误驱动共享重试
    // 预算（§5），目标 nullopt = 跟随新默认；不改变路由模式。
    if (!consume_restart_budget()) {
        const SwitchResult switch_result { SwitchOutcome::Fatal, AudioError::BackendFailed };
        last_switch_result_.store(switch_result, std::memory_order_release);
        state_.store(CaptureSwitchState::Fatal, std::memory_order_release);
        log_error("CaptureManager: auto-restart retry budget exhausted (10s/3) on default-device follow");
        return;
    }
    (void)switch_to(std::nullopt);
}

void CaptureManager::stop() noexcept
{
    log_debug("CaptureManager stop: tearing down capture stream");
    if (capture_) {
        capture_->stop();
    }
    active_device_.reset();
    state_.store(CaptureSwitchState::Inactive, std::memory_order_release);
}

} // namespace aqua::audio
