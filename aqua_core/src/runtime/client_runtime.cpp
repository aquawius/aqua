#include "aqua/runtime/client_runtime.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <exception>
#include <limits>
#include <system_error>

namespace aqua::runtime {

ClientRuntime::ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , grpc_()
    , udp_(ioc)
    , callback_gate_(std::make_shared<CallbackGate>(this))
{
    log_debug("ClientRuntime instance created");
}

ClientRuntime::~ClientRuntime()
{
    stop();
    if (callback_gate_) {
        callback_gate_->detach();
    }
}

bool ClientRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    const bool entered = state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (entered) {
        log_debug("ClientRuntime state: Created -> Starting");
    }
    return entered;
}

bool ClientRuntime::enter_stopping() noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Stopping || state == RuntimeState::Stopped) {
            return false;
        }
        if (state_.compare_exchange_weak(state, RuntimeState::Stopping,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            log_debug("ClientRuntime state: -> Stopping");
            return true;
        }
    }
}

void ClientRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
    log_debug("ClientRuntime state: -> Stopped");
}

bool ClientRuntime::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!enter_starting()) {
        return false;
    }
    if (config_.jitter_buffer_slots < config::MIN_JITTER_BUFFER_SLOTS
        || config_.jitter_buffer_slots > config::MAX_JITTER_BUFFER_SLOTS) {
        log_error_fmt("ClientRuntime: jitter_buffer_slots must be {}..{}",
            config::MIN_JITTER_BUFFER_SLOTS, config::MAX_JITTER_BUFFER_SLOTS);
        stop_locked();
        return false;
    }

    if (config_.hello_interval <= std::chrono::milliseconds(0)) {
        log_error_fmt("ClientRuntime: invalid configuration: hello_interval={}ms must be > 0",
            config_.hello_interval.count());
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime config: server={} client_name='{}' jitter_slots={} hello_interval={}ms playback_device={} playback_buffer_frames={} playback_low_latency={} hold_current={}",
        aqua::net::format_host_port(config_.server_ip, config_.rpc_port), config_.client_name, config_.jitter_buffer_slots,
        config_.hello_interval.count(),
        config_.playback.device ? config_.playback.device->value() : std::string("default"),
        config_.playback.frames_per_buffer,
        config_.playback.low_latency,
        config_.playback_hold_current);

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        log_error("ClientRuntime: audio device manager is unavailable on this platform");
        stop_locked();
        return false;
    }
    playback_ = std::make_unique<audio::PlaybackManager>(*device_mgr_);
    if (!playback_->available()) {
        log_error("ClientRuntime: audio playback backend is unavailable on this platform");
        stop_locked();
        return false;
    }

    connect_result_ = { };
    log_info_fmt("ClientRuntime: connecting to gRPC server {}", aqua::net::format_host_port(config_.server_ip, config_.rpc_port));
    if (!grpc_.connect_to_server(config_.server_ip, config_.rpc_port)
        || !grpc_.connect(config_.client_name, connect_result_)
        || !connect_result_.is_valid()) {
        log_error_fmt("ClientRuntime: control-plane connection to {} failed",
            aqua::net::format_host_port(config_.server_ip, config_.rpc_port));
        stop_locked();
        return false;
    }

    if (!connect_result_.audio_format.is_valid()) {
        log_error("ClientRuntime: server returned invalid audio format");
        stop_locked();
        return false;
    }
    if (connect_result_.frame_count == 0) {
        log_error("ClientRuntime: server returned frame_count=0");
        stop_locked();
        return false;
    }
    const auto remote_frame_bytes = connect_result_.audio_format.frame_bytes();
    if (remote_frame_bytes == 0
        || static_cast<std::size_t>(connect_result_.frame_count)
            > std::numeric_limits<std::size_t>::max() / remote_frame_bytes) {
        log_error_fmt("ClientRuntime: remote frame geometry overflows (frame_count={} frame_bytes={})",
            connect_result_.frame_count, remote_frame_bytes);
        stop_locked();
        return false;
    }
    const auto expected_payload_bytes = static_cast<std::size_t>(connect_result_.frame_count) * remote_frame_bytes;
    if (expected_payload_bytes == 0 || expected_payload_bytes > aqua::config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ClientRuntime: received frame payload {} bytes exceeds UDP safe payload budget {} bytes",
            expected_payload_bytes, aqua::config::UDP_AUDIO_PAYLOAD_BYTES);
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime: validated remote stream geometry: payload_bytes={} jitter_slots={}",
        expected_payload_bytes, config_.jitter_buffer_slots);
    if (!setup_playback(connect_result_.audio_format, connect_result_.frame_count)) {
        log_error("ClientRuntime: failed to create playback/JitterBuffer pipeline");
        stop_locked();
        return false;
    }
    log_debug_fmt("ClientRuntime playback/JitterBuffer pipeline ready: frame_count={} frame_bytes={} jb_slots={}",
        frame_count_, frame_bytes_, config_.jitter_buffer_slots);

    const auto effective_udp_port = config_.force_udp_port.value_or(connect_result_.advertised_udp_port);
    if (config_.force_udp_port) {
        log_info_fmt("ClientRuntime: overriding Server-advertised UDP port {} with forced port {}",
            connect_result_.advertised_udp_port, *config_.force_udp_port);
    }
    if (!udp_.set_remote(connect_result_.advertised_udp_address, effective_udp_port)) {
        log_error_fmt("ClientRuntime: failed to configure UDP remote {}",
            aqua::net::format_host_port(connect_result_.advertised_udp_address, effective_udp_port));
        stop_locked();
        return false;
    }

    log_debug_fmt("ClientRuntime: starting UDP receive, expected payload={} bytes", expected_payload_bytes);
    if (!udp_.start_receive(expected_payload_bytes,
            [gate = callback_gate_, jb = jb_, frame_count = frame_count_](
                std::uint64_t sequence, std::span<const std::byte> pcm) {
                const audio::AudioFrame frame { sequence, frame_count, pcm };
                (void)jb->push(frame);
                const auto rejected = jb->take_reanchor_sanity_rejections();
                if (rejected != 0) {
                    gate->invoke([rejected](ClientRuntime& owner) noexcept {
                        owner.on_reanchor_sanity_failure(rejected);
                    });
                }
            })) {
        log_error("ClientRuntime: failed to start UDP receive loop");
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime UDP receive loop started");
    log_debug_fmt("ClientRuntime: starting HELLO keepalive, interval={}ms", config_.hello_interval.count());
    if (!udp_.start_hello(connect_result_.session_id, config_.hello_interval,
            [gate = callback_gate_](std::uint32_t misses) noexcept {
                gate->invoke([misses](ClientRuntime& owner) noexcept {
                    owner.on_network_liveness_failure(misses);
                });
            })) {
        log_error("ClientRuntime: failed to start HELLO keepalive");
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime HELLO keepalive started");

    auto pb_cfg = config_.playback;
    pb_cfg.format = connect_result_.audio_format;
    // 路由起步（连接属性）：hold_current = "自动切换播放设备"关。
    playback_->set_hold_current_on_start(config_.playback_hold_current);
    const auto playback_start = playback_->start(pb_cfg, [this](std::span<std::byte> output) noexcept { return pull_playback(output); }, [this](audio::AudioError error) noexcept { on_playback_event(error); });
    if (!playback_start) {
        log_error_fmt("ClientRuntime: failed to start audio playback: {}",
            audio::audio_error_name(playback_start.error()));
        stop_locked();
        return false;
    }
    log_debug("ClientRuntime playback backend started");

    RuntimeState expected = RuntimeState::Starting;
    (void)state_.compare_exchange_strong(expected, RuntimeState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);
    const auto final_state = state_.load(std::memory_order_acquire);
    log_debug_fmt("ClientRuntime startup completed with state={}", runtime_state_name(final_state));
    if (final_state == RuntimeState::Running || final_state == RuntimeState::Degraded) {
        log_info_fmt("ClientRuntime started: session=0x{:08X} server={} audio={}ch/{}Hz F={} JB={} slots",
            connect_result_.session_id,
            connect_result_.advertised_udp_address,
            connect_result_.audio_format.channels,
            connect_result_.audio_format.sample_rate,
            connect_result_.frame_count,
            config_.jitter_buffer_slots);
        return true;
    }
    return false;
}

void ClientRuntime::stop() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    stop_locked();
}

void ClientRuntime::stop_locked() noexcept
{
    log_debug("ClientRuntime stop requested");
    if (!enter_stopping()) {
        return;
    }

    if (playback_) {
        log_debug("ClientRuntime stopping playback backend");
        playback_->stop();
    }
    log_debug("ClientRuntime stopping UDP transport");
    udp_.stop();
    if (connect_result_.session_id != 0) {
        log_debug_fmt("ClientRuntime disconnecting session=0x{:08X}", connect_result_.session_id);
        try {
            (void)grpc_.disconnect(connect_result_.session_id);
        } catch (const std::system_error& e) {
            log_debug_fmt("ClientRuntime disconnect threw during stop: code={} message={}",
                e.code().value(), format_system_error_message(e.code()));
        } catch (const std::exception& e) {
            log_debug_fmt("ClientRuntime disconnect threw during stop: {}", format_exception_message(e));
        } catch (...) {
            log_debug("ClientRuntime disconnect threw during stop");
        }
    }
    enter_stopped();
    log_info("ClientRuntime stopped");
}

double ClientRuntime::jitter_water_level() const noexcept
{
    return jb_ ? jb_->water_level() : 0.0;
}

std::uint32_t ClientRuntime::jitter_used_slots() const noexcept
{
    return jb_ ? jb_->used_slots() : 0;
}

std::uint32_t ClientRuntime::jitter_capacity_slots() const noexcept
{
    return jb_ ? jb_->capacity_slots() : 0;
}

std::uint64_t ClientRuntime::jitter_reanchor_count() const noexcept
{
    return jb_ ? jb_->reanchor_count() : 0;
}

std::uint64_t ClientRuntime::jitter_reanchor_sanity_rejections() const noexcept
{
    return jb_ ? jb_->reanchor_sanity_rejections() : 0;
}

std::uint64_t ClientRuntime::jitter_last_reanchor_sequence() const noexcept
{
    if (!jb_ || jb_->reanchor_count() == 0) {
        return 0;
    }
    return jb_->last_reanchor_sequence();
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frame_count)
{
    if (state_.load(std::memory_order_acquire) != RuntimeState::Starting) {
        log_error("ClientRuntime::setup_playback: not in Starting state");
        return false;
    }
    if (!format.is_valid() || frame_count == 0) {
        log_error_fmt("ClientRuntime::setup_playback: invalid geometry (format_valid={} frame_count={})",
            format.is_valid(), frame_count);
        return false;
    }
    const auto frame_bytes = format.frame_bytes();
    if (frame_bytes == 0
        || static_cast<std::size_t>(frame_count)
            > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        log_error_fmt("ClientRuntime::setup_playback: frame geometry overflows (frame_count={} frame_bytes={})",
            frame_count, frame_bytes);
        return false;
    }
    frame_count_ = frame_count;
    frame_bytes_ = frame_bytes;
    audio::JitterBufferConfig cfg;
    cfg.capacity_slots = config_.jitter_buffer_slots;
    cfg.format = format;
    cfg.frame_count = frame_count;
    auto jb = audio::JitterBuffer::create(cfg);
    if (!jb) {
        jb_.reset();
        log_error_fmt("ClientRuntime::setup_playback: JitterBuffer create failed: {}",
            audio::audio_error_name(jb.error()));
        return false;
    }
    jb_ = std::move(*jb);
    return true;
}

std::uint64_t ClientRuntime::jitter_push_accepted() const noexcept { return jb_ ? jb_->push_accepted() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected() const noexcept { return jb_ ? jb_->push_rejected() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_late() const noexcept { return jb_ ? jb_->push_rejected_late() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_slot_busy() const noexcept { return jb_ ? jb_->push_rejected_slot_busy() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_invalid() const noexcept { return jb_ ? jb_->push_rejected_invalid() : 0; }
std::uint64_t ClientRuntime::jitter_push_rejected_sanity() const noexcept { return jb_ ? jb_->push_rejected_sanity() : 0; }
std::uint64_t ClientRuntime::jitter_pull_calls() const noexcept { return jb_ ? jb_->pull_calls() : 0; }
std::uint64_t ClientRuntime::jitter_pull_frames() const noexcept { return jb_ ? jb_->pull_frames() : 0; }
std::uint64_t ClientRuntime::jitter_pull_silence_frames() const noexcept { return jb_ ? jb_->pull_silence_frames() : 0; }
std::uint64_t ClientRuntime::jitter_fill_episodes() const noexcept { return jb_ ? jb_->fill_episodes() : 0; }
std::uint64_t ClientRuntime::jitter_fill_corrected_slots() const noexcept { return jb_ ? jb_->fill_corrected_slots() : 0; }
std::uint64_t ClientRuntime::jitter_drop_episodes() const noexcept { return jb_ ? jb_->drop_episodes() : 0; }
std::uint64_t ClientRuntime::jitter_drop_skipped_slots() const noexcept { return jb_ ? jb_->drop_skipped_slots() : 0; }
std::uint64_t ClientRuntime::jitter_reanchor_requests() const noexcept { return jb_ ? jb_->reanchor_requests() : 0; }
std::uint64_t ClientRuntime::jitter_reanchor_cancels() const noexcept { return jb_ ? jb_->reanchor_cancels() : 0; }
std::uint64_t ClientRuntime::playback_pull_calls() const noexcept { return playback_pull_calls_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_frames() const noexcept { return playback_pull_frames_.load(std::memory_order_relaxed); }
std::uint64_t ClientRuntime::playback_pull_silence_frames() const noexcept { return playback_pull_silence_frames_.load(std::memory_order_relaxed); }

void ClientRuntime::on_playback_event(audio::AudioError error) noexcept
{
    if (error == audio::AudioError::None) {
        return;
    }
    last_audio_error_.store(error, std::memory_order_release);

    // 设备类错误（拔出/不可用/消失）：置标志并立即把派发请求 post 到 ioc
    // （§7：本回调运行在 backend event 线程，restart 的 stop/join/start
    // 不得在此执行；不等待控制线程的下一个 500ms tick——检测延迟是设备
    // 切换静音期的大头）。派发经 callback_gate_（析构时 detach），ioc 上
    // 残留的任务不会触碰已销毁的 runtime；service_playback_recovery 在
    // ioc 线程就地执行 restart 事务（stop+start，JB 不清空 = 结转）。
    if (error == audio::AudioError::DeviceDisconnected
        || error == audio::AudioError::DeviceUnavailable
        || error == audio::AudioError::DeviceNotFound) {
        playback_device_error_pending_.store(true, std::memory_order_release);
        log_warn_fmt("client runtime: device error {}, recovery dispatching",
            audio::audio_error_name(error));
        asio::post(ioc_, [gate = callback_gate_]() noexcept {
            gate->invoke([](ClientRuntime& owner) noexcept {
                owner.service_playback_recovery();
            });
        });
        return;
    }

    // 其余错误（格式/后端内部错误等）：保持既有 Degraded 语义。
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: {}", audio::audio_error_name(error));
                return;
            }
            continue;
        }
        return;
    }
}

void ClientRuntime::service_playback_recovery() noexcept
{
    // 快速路径：无设备错误标志时不加锁（ioc 即时派发 + supervision 兜底轮询）。
    if (!playback_device_error_pending_.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard lock(lifecycle_mutex_);
    // 锁内双检查（可能已被并发消费）。
    if (!playback_device_error_pending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // 仅会话运行中恢复：Stopping/Stopped/Degraded 交给既有终止路径。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return;
    }
    if (!playback_) {
        return;
    }
    log_info("client runtime: starting error-driven playback recovery");
    // 链耗尽 → PlaybackState=Fatal；supervision 下一 tick 按 Fatal 终止。
    (void)playback_->restart_on_error();
}

void ClientRuntime::service_default_device_follow() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    // 仅会话运行中跟随；Stopping/Stopped/Degraded 交给既有终止路径。
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return;
    }
    if (!playback_) {
        return;
    }
    // 设备轮询与切换决策在 PlaybackManager::tick()（它持 AudioDeviceManager
    // 引用，负责 FollowSystem 的默认设备变化检测）；本方法只做生命周期门禁 +
    // lifecycle_mutex_ 串行化后转发，ClientRuntime 不感知具体设备语义。
    playback_->tick();
}

std::expected<audio::SwitchResult, audio::AudioError>
ClientRuntime::set_playback_device(std::optional<audio::AudioDeviceId> target) noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != RuntimeState::Running) {
        return std::unexpected(audio::AudioError::NotRunning);
    }
    if (!playback_) {
        return std::unexpected(audio::AudioError::NotRunning);
    }
    return playback_->set_playback_device(std::move(target));
}

void ClientRuntime::on_network_liveness_failure(std::uint32_t consecutive_misses) noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: no HELLO_ACK for {} consecutive intervals",
                    consecutive_misses);
                return;
            }
            continue;
        }
        return;
    }
}

void ClientRuntime::on_reanchor_sanity_failure(std::uint64_t rejections) noexcept
{
    if (rejections == 0) {
        return;
    }
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Starting || state == RuntimeState::Running) {
            if (state_.compare_exchange_weak(state, RuntimeState::Degraded,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                log_warn_fmt("client runtime degraded: rejected {} absurd JitterBuffer reanchor requests",
                    rejections);
                return;
            }
            continue;
        }
        return;
    }
}

std::uint32_t ClientRuntime::pull_playback(std::span<std::byte> output) noexcept
{
    if (jb_ == nullptr) {
        return 0;
    }
    const auto result = jb_->pull(output);
    playback_pull_calls_.fetch_add(1, std::memory_order_relaxed);
    playback_pull_frames_.fetch_add(result.frames_filled, std::memory_order_relaxed);
    playback_pull_silence_frames_.fetch_add(result.silence_frames, std::memory_order_relaxed);
    return result.frames_filled;
}

aqua::diagnostics::ClientDiagnosticsSnapshot ClientRuntime::take_diagnostics_snapshot() const noexcept
{
    aqua::diagnostics::ClientDiagnosticsSnapshot snapshot;
    snapshot.state = state_.load(std::memory_order_acquire);
    snapshot.last_audio_error = last_audio_error_.load(std::memory_order_acquire);
    snapshot.playback_running = playback_running();
    snapshot.playback_state = playback_state();
    if (playback_ != nullptr) {
        snapshot.route_mode = playback_->route_mode();
        snapshot.switch_result = playback_->last_switch_result().value_or(
            audio::SwitchResult { });
        snapshot.requested_device_id = playback_->requested_device().value_or(
            audio::AudioDeviceId { });
    }

    snapshot.net.hello_ack_count = udp_.hello_ack_count();
    snapshot.net.hello_ack_misses = udp_.consecutive_hello_ack_misses();
    snapshot.net.hello_ack_age_ms = udp_.hello_ack_age_ms();
    snapshot.net.hello_failed = udp_.hello_failed();
    snapshot.net.hello_send_attempts = udp_.hello_send_attempts();
    snapshot.net.hello_ack_miss_events = udp_.hello_ack_miss_events();
    snapshot.net.transport = udp_.stats();
    snapshot.net.audio_frames_accepted = udp_.audio_frames_accepted();
    snapshot.net.malformed_datagrams = udp_.malformed_datagrams();
    snapshot.net.unexpected_sender_datagrams = udp_.unexpected_sender_datagrams();
    snapshot.net.wrong_session_acks = udp_.wrong_session_acks();
    snapshot.net.audio_payload_mismatches = udp_.audio_payload_mismatches();
    snapshot.net.non_audio_datagrams = udp_.non_audio_datagrams();

    auto& jb = snapshot.jitter_buffer;
    if (jb_ != nullptr) {
        jb.water_level = jb_->water_level();
        jb.used_slots = jb_->used_slots();
        jb.capacity_slots = jb_->capacity_slots();
        jb.reanchor_count = jb_->reanchor_count();
        jb.reanchor_requests = jb_->reanchor_requests();
        jb.reanchor_cancels = jb_->reanchor_cancels();
        jb.reanchor_sanity_rejections = jb_->reanchor_sanity_rejections();
        jb.last_reanchor_sequence = jitter_last_reanchor_sequence();
        jb.push_accepted = jb_->push_accepted();
        jb.push_rejected = jb_->push_rejected();
        jb.push_rejected_late = jb_->push_rejected_late();
        jb.push_rejected_slot_busy = jb_->push_rejected_slot_busy();
        jb.push_rejected_invalid = jb_->push_rejected_invalid();
        jb.push_rejected_sanity = jb_->push_rejected_sanity();
        jb.pull_calls = jb_->pull_calls();
        jb.pull_frames = jb_->pull_frames();
        jb.pull_silence_frames = jb_->pull_silence_frames();
        jb.fill_episodes = jb_->fill_episodes();
        jb.fill_corrected_slots = jb_->fill_corrected_slots();
        jb.drop_episodes = jb_->drop_episodes();
        jb.drop_skipped_slots = jb_->drop_skipped_slots();
    }

    snapshot.playback.pull_calls = playback_pull_calls();
    snapshot.playback.pull_frames = playback_pull_frames();
    snapshot.playback.pull_silence_frames = playback_pull_silence_frames();

    snapshot.stream = playback_ ? playback_->stream_info() : audio::AudioStreamInfo { };
    return snapshot;
}

} // namespace aqua::runtime
