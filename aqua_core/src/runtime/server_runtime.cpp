#include "aqua/runtime/server_runtime.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <limits>
#include <new>
#include <system_error>

namespace aqua::runtime {

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , sessions_(std::make_shared<session::SessionManager>())
    , udp_(ioc, sessions_)
    , packetizer_(config.frame_count, config.format.frame_bytes())
    , frame_queue_(config.network_queue_slots, config.frame_count, config.format.frame_bytes())
    , dispatcher_(frame_queue_, udp_)
{
    log_debug_fmt("ServerRuntime instance created: network_queue_slots={} frame_count={} frame_bytes={}",
        config_.network_queue_slots, config_.frame_count, config_.format.frame_bytes());
}

ServerRuntime::~ServerRuntime()
{
    stop();
}

bool ServerRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    const bool entered = state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (entered) {
        log_debug("ServerRuntime state: Created -> Starting");
    }
    return entered;
}

bool ServerRuntime::enter_stopping() noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    for (;;) {
        if (state == RuntimeState::Stopping || state == RuntimeState::Stopped) {
            return false;
        }
        if (state_.compare_exchange_weak(state, RuntimeState::Stopping,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            log_debug("ServerRuntime state: -> Stopping");
            return true;
        }
    }
}

void ServerRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
    log_debug("ServerRuntime state: -> Stopped");
}

bool ServerRuntime::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (!enter_starting()) {
        return false;
    }
    if (weak_from_this().expired()) {
        log_error("ServerRuntime must be owned by std::shared_ptr");
        stop_locked();
        return false;
    }

    const auto frame_bytes = config_.format.frame_bytes();
    // Validate user/configuration-facing invariants individually so the first visible
    // startup failure always carries an actionable reason at Error level.
    if (!config_.format.is_valid()) {
        log_error("ServerRuntime: invalid server audio format");
        stop_locked();
        return false;
    }
    if (config_.frame_count == 0) {
        log_error("ServerRuntime: frame_count must be > 0");
        stop_locked();
        return false;
    }
    if (config_.network_queue_slots == 0) {
        log_error("ServerRuntime: network_queue_slots must be > 0");
        stop_locked();
        return false;
    }
    if (config_.session_timeout <= std::chrono::milliseconds(0)) {
        log_error_fmt("ServerRuntime: session_timeout={}ms must be > 0", config_.session_timeout.count());
        stop_locked();
        return false;
    }
    if (config_.session_reap_interval <= std::chrono::milliseconds(0)) {
        log_error_fmt("ServerRuntime: session_reap_interval={}ms must be > 0", config_.session_reap_interval.count());
        stop_locked();
        return false;
    }
    if (config_.rpc_port == 0) {
        log_error("ServerRuntime: rpc_port must be > 0");
        stop_locked();
        return false;
    }
    if (!packetizer_.valid() || !frame_queue_.valid() || frame_bytes == 0) {
        log_error("ServerRuntime: invalid audio/network queue geometry");
        stop_locked();
        return false;
    }
    if (static_cast<std::size_t>(config_.frame_count)
        > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        log_error("ServerRuntime: frame_count × frame_bytes overflows size_t");
        stop_locked();
        return false;
    }

    try {
        (void)::aqua::net::parse_ip_address(config_.advertised_udp_address);
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: invalid advertised UDP address '{}' - {}",
            config_.advertised_udp_address, e.what());
        stop_locked();
        return false;
    }

    const auto payload_bytes = static_cast<std::size_t>(config_.frame_count) * frame_bytes;
    if (payload_bytes > config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ServerRuntime: AudioFrame payload {} exceeds UDP safe payload budget {}",
            payload_bytes, config::UDP_AUDIO_PAYLOAD_BYTES);
        stop_locked();
        return false;
    }

    try {
        reap_state_ = std::make_shared<ReapState>(ioc_);
    } catch (const std::bad_alloc&) {
        log_error("ServerRuntime: failed to allocate session reaper state");
        stop_locked();
        return false;
    }

    log_debug_fmt("ServerRuntime config: udp_bind={}:{} rpc_bind={}:{} advertised_udp={}:{} format={}ch/{}Hz/enc={} frame_count={} queue_slots={} session_timeout={}ms reap_interval={}ms capture_source={} device={}",
        config_.udp_bind_ip, config_.udp_port, config_.rpc_bind_ip, config_.rpc_port,
        config_.advertised_udp_address, config_.udp_port,
        config_.format.channels, config_.format.sample_rate, static_cast<int>(config_.format.encoding),
        config_.frame_count, config_.network_queue_slots,
        config_.session_timeout.count(), config_.session_reap_interval.count(),
        static_cast<int>(config_.capture.source),
        config_.capture.device ? config_.capture.device->value() : std::string("default"));

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        log_error("ServerRuntime: audio device manager is unavailable on this platform");
        stop_locked();
        return false;
    }
    capture_ = audio::create_capture(*device_mgr_);
    log_debug("ServerRuntime: audio device manager and capture backend created");
    if (!capture_) {
        log_error("ServerRuntime: audio capture backend is unavailable on this platform");
        stop_locked();
        return false;
    }

    if (!udp_.bind(config_.udp_bind_ip, config_.udp_port)) {
        log_error_fmt("ServerRuntime: failed to bind UDP {}:{}",
            config_.udp_bind_ip, config_.udp_port);
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime UDP ready: local_endpoint={}:{}",
        config_.udp_bind_ip, udp_.local_endpoint().port());
    if (!udp_.start()) {
        log_error("ServerRuntime: failed to start UDP receive loop");
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime UDP receive loop started on {}",
        ::aqua::net::format_host_port(config_.udp_bind_ip, udp_.local_endpoint().port()));
    if (!dispatcher_.start()) {
        log_error("ServerRuntime: failed to start audio network dispatcher");
        stop_locked();
        return false;
    }

    grpc_ = std::make_unique<grpc::GrpcServer>(
        *sessions_, config_.format, config_.frame_count,
        config_.rpc_bind_ip, config_.rpc_port,
        grpc::AdvertisedUdpEndpoint { config_.advertised_udp_address, udp_.local_endpoint().port() });
    if (!grpc_->is_started()) {
        log_error("ServerRuntime: gRPC server failed to start");
        stop_locked();
        return false;
    }
    log_debug("ServerRuntime gRPC server constructed and ready; starting worker thread");
    try {
        grpc_thread_ = std::thread([this] { grpc_->run(); });
    } catch (const std::system_error& e) {
        log_error_fmt("ServerRuntime: failed to start gRPC worker thread: code={} message={}",
            e.code().value(), format_system_error_message(e.code()));
        stop_locked();
        return false;
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: failed to start gRPC worker thread: {}", e.what());
        stop_locked();
        return false;
    } catch (...) {
        log_error("ServerRuntime: failed to start gRPC worker thread");
        stop_locked();
        return false;
    }

    auto capture_cfg = config_.capture;
    capture_cfg.format = config_.format;
    const auto capture_start = capture_->start(capture_cfg,
        [this](const audio::AudioBlock& block) noexcept { on_capture_block(block); },
        [this](audio::AudioError error) noexcept { on_capture_event(error); });
    if (!capture_start) {
        log_error_fmt("ServerRuntime: failed to start audio capture: {}",
            audio::audio_error_name(capture_start.error()));
        stop_locked();
        return false;
    }
    log_debug_fmt("ServerRuntime capture started: format={}ch/{}Hz frame_count={} source={}",
        capture_cfg.format->channels, capture_cfg.format->sample_rate,
        config_.frame_count, static_cast<int>(capture_cfg.source));

    RuntimeState expected = RuntimeState::Starting;
    (void)state_.compare_exchange_strong(expected, RuntimeState::Running,
        std::memory_order_acq_rel, std::memory_order_acquire);
    const auto reap = reap_state_;
    if (reap) {
        const auto weak_self = weak_from_this();
        try {
            asio::post(reap->strand, [reap, weak_self, interval = config_.session_reap_interval,
                timeout = config_.session_timeout] {
                schedule_reap(reap, weak_self, interval, timeout);
            });
        } catch (const std::exception& e) {
            log_error_fmt("ServerRuntime: failed to schedule session reaper: {}", e.what());
            stop_locked();
            return false;
        } catch (...) {
            log_error("ServerRuntime: failed to schedule session reaper");
            stop_locked();
            return false;
        }
    }
    const auto final_state = state_.load(std::memory_order_acquire);
    log_debug_fmt("ServerRuntime startup completed with state={}", runtime_state_name(final_state));
    if (final_state == RuntimeState::Running || final_state == RuntimeState::Degraded) {
        log_info_fmt("ServerRuntime started: rpc={}:{} udp={}:{} audio={}ch/{}Hz F={} queue={} slots",
            config_.rpc_bind_ip, config_.rpc_port, config_.advertised_udp_address,
            udp_.local_endpoint().port(), config_.format.channels, config_.format.sample_rate,
            config_.frame_count, config_.network_queue_slots);
        return true;
    }
    return false;
}

void ServerRuntime::stop() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    stop_locked();
}

void ServerRuntime::stop_locked() noexcept
{
    log_debug("ServerRuntime stop requested");
    if (!enter_stopping()) {
        return;
    }

    if (capture_) {
        log_debug("ServerRuntime stopping capture backend");
        capture_->stop();
    }
    if (reap_state_ != nullptr) {
        const auto reap = reap_state_;
        try {
            asio::post(reap->strand, [reap] {
                asio::error_code ec;
                reap->timer->cancel(ec);
            });
        } catch (...) {
            // 定时器由 State 共享并归 strand 所有；若 executor 已停止，
            // 最后一个 shared 状态的析构会取消未完成的等待。
        }
    }
    log_debug("ServerRuntime stopping audio network dispatcher");
    dispatcher_.stop();
    log_debug("ServerRuntime stopping UDP transport");
    udp_.stop();
    if (grpc_) {
        log_debug("ServerRuntime shutting down gRPC server");
        grpc_->shutdown();
    }
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
    sessions_->clear();
    enter_stopped();
    log_info("ServerRuntime stopped");
}

void ServerRuntime::on_capture_block(const audio::AudioBlock& block) noexcept
{
    const auto state = state_.load(std::memory_order_acquire);
    if ((state != RuntimeState::Starting && state != RuntimeState::Running
            && state != RuntimeState::Degraded)
        || block.data.empty()) {
        return;
    }

    packetizer_.push(block.data, [this](const audio::AudioFrame& frame) noexcept {
        const auto result = frame_queue_.push(frame);
        if (result.accepted) {
            dispatcher_.publish_from_realtime(result.should_notify);
        }
    });
}

void ServerRuntime::on_capture_event(audio::AudioError error) noexcept
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
                log_warn_fmt("server runtime degraded: {}", audio::audio_error_name(error));
                return;
            }
            continue;
        }
        return;
    }
}

void ServerRuntime::schedule_reap(const std::shared_ptr<ReapState>& reap,
    const std::weak_ptr<ServerRuntime>& weak_self,
    std::chrono::milliseconds interval, std::chrono::milliseconds timeout)
{
    if (!reap || !reap->timer) {
        return;
    }
    reap->timer->expires_after(interval);
    reap->timer->async_wait(asio::bind_executor(reap->strand,
        [reap, weak_self, interval, timeout](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            const auto self = weak_self.lock();
            if (!self) {
                return;
            }
            const auto state = self->state_.load(std::memory_order_acquire);
            if (state != RuntimeState::Running && state != RuntimeState::Degraded) {
                return;
            }
            self->sessions_->remove_expired_sessions(timeout);
            schedule_reap(reap, weak_self, interval, timeout);
        }));
}

} // namespace aqua::runtime
