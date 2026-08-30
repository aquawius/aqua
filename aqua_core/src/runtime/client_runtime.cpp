#include "aqua/runtime/client_runtime.h"

#include "aqua/logger/logger.h"

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

    log_debug_fmt("ClientRuntime config: server={}:{} client_name='{}' jitter_slots={} hello_interval={}ms playback_device={} playback_buffer_frames={}",
        config_.server_ip, config_.rpc_port, config_.client_name, config_.jitter_buffer_slots,
        config_.hello_interval.count(),
        config_.playback.device ? config_.playback.device->value() : std::string("default"),
        config_.playback.frames_per_buffer);

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        log_error("ClientRuntime: audio device manager is unavailable on this platform");
        stop_locked();
        return false;
    }
    playback_ = audio::create_playback(*device_mgr_);
    if (!playback_) {
        log_error("ClientRuntime: audio playback backend is unavailable on this platform");
        stop_locked();
        return false;
    }

    connect_result_ = { };
    log_info_fmt("ClientRuntime: connecting to gRPC server {}:{}", config_.server_ip, config_.rpc_port);
    if (!grpc_.connect_to_server(config_.server_ip, config_.rpc_port)
        || !grpc_.connect(config_.client_name, connect_result_)
        || !connect_result_.is_valid()) {
        log_error_fmt("ClientRuntime: control-plane connection to {}:{} failed",
            config_.server_ip, config_.rpc_port);
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

    const auto effective_udp_port = config_.force_udp_port.value_or(connect_result_.udp_port);
    if (config_.force_udp_port) {
        log_info_fmt("ClientRuntime: overriding Server-advertised UDP port {} with forced port {}",
            connect_result_.udp_port, *config_.force_udp_port);
    }
    if (!udp_.set_remote(connect_result_.udp_address, effective_udp_port)) {
        log_error_fmt("ClientRuntime: failed to configure UDP remote {}:{}",
            connect_result_.udp_address, effective_udp_port);
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
            connect_result_.udp_address,
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

} // namespace aqua::runtime
