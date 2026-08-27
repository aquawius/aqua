#include "aqua/runtime/client_runtime.h"

#include "aqua/logger/logger.h"

#include <limits>

namespace aqua::runtime {

ClientRuntime::ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , grpc_()
    , udp_(ioc)
{
}

ClientRuntime::~ClientRuntime()
{
    stop();
}

bool ClientRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    return state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
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
            return true;
        }
    }
}

void ClientRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
}

bool ClientRuntime::start()
{
    if (!enter_starting()) {
        return false;
    }

    if (config_.jitter_buffer_slots == 0
        || config_.hello_interval <= std::chrono::milliseconds(0)) {
        stop();
        return false;
    }

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        stop();
        return false;
    }
    playback_ = audio::create_playback(*device_mgr_);
    if (!playback_) {
        stop();
        return false;
    }

    connect_result_ = {};
    if (!grpc_.connect_to_server(config_.server_ip, config_.rpc_port)
        || !grpc_.connect(config_.client_name, connect_result_)
        || !connect_result_.is_valid()) {
        stop();
        return false;
    }

    if (!setup_playback(connect_result_.audio_format, connect_result_.frame_count)) {
        stop();
        return false;
    }

    const auto expected_payload_bytes = static_cast<std::size_t>(frame_count_) * frame_bytes_;
    if (expected_payload_bytes == 0 || expected_payload_bytes > config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ClientRuntime: received frame size {} exceeds UDP safe payload budget {}",
            expected_payload_bytes, config::UDP_AUDIO_PAYLOAD_BYTES);
        stop();
        return false;
    }

    if (!udp_.set_remote(connect_result_.udp_address, connect_result_.udp_port)) {
        stop();
        return false;
    }

    if (!udp_.start_receive(expected_payload_bytes,
            [jb = jb_, frame_count = frame_count_](std::uint64_t sequence,
                std::span<const std::byte> pcm) {
                const audio::AudioFrame frame { sequence, frame_count, pcm };
                (void)jb->push(frame);
            })) {
        stop();
        return false;
    }
    udp_.start_hello(connect_result_.session_id, config_.hello_interval);

    auto pb_cfg = config_.playback;
    pb_cfg.format = connect_result_.audio_format;
    if (!playback_->start(pb_cfg,
            [this](std::span<std::byte> output) noexcept { return pull_playback(output); },
            [this](audio::AudioError error) noexcept { on_playback_event(error); })) {
        stop();
        return false;
    }

    RuntimeState expected = RuntimeState::Starting;
    (void)state_.compare_exchange_strong(expected, RuntimeState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);
    const auto final_state = state_.load(std::memory_order_acquire);
    return final_state == RuntimeState::Running || final_state == RuntimeState::Degraded;
}

void ClientRuntime::stop() noexcept
{
    if (!enter_stopping()) {
        return;
    }

    if (playback_) {
        playback_->stop();
    }
    udp_.stop();
    if (connect_result_.is_valid()) {
        (void)grpc_.disconnect(connect_result_.session_id);
    }
    enter_stopped();
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frame_count)
{
    if (state_.load(std::memory_order_acquire) != RuntimeState::Starting
        || !format.is_valid() || frame_count == 0) {
        return false;
    }
    const auto frame_bytes = format.frame_bytes();
    if (frame_bytes == 0
        || static_cast<std::size_t>(frame_count)
            > std::numeric_limits<std::size_t>::max() / frame_bytes) {
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
        return false;
    }
    jb_ = std::move(*jb);
    return true;
}

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

std::uint32_t ClientRuntime::pull_playback(std::span<std::byte> output) noexcept
{
    if (jb_ == nullptr) {
        return 0;
    }
    return jb_->pull(output).frames_filled;
}

} // namespace aqua::runtime
