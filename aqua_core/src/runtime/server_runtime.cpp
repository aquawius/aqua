#include "aqua/runtime/server_runtime.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <limits>
#include <new>

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
}

ServerRuntime::~ServerRuntime()
{
    stop();
}

bool ServerRuntime::enter_starting() noexcept
{
    RuntimeState expected = RuntimeState::Created;
    return state_.compare_exchange_strong(expected, RuntimeState::Starting,
        std::memory_order_acq_rel, std::memory_order_acquire);
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
            return true;
        }
    }
}

void ServerRuntime::enter_stopped() noexcept
{
    state_.store(RuntimeState::Stopped, std::memory_order_release);
}

bool ServerRuntime::start()
{
    if (!enter_starting()) {
        return false;
    }
    if (weak_from_this().expired()) {
        log_error("ServerRuntime must be owned by std::shared_ptr");
        stop();
        return false;
    }

    const auto frame_bytes = config_.format.frame_bytes();
    if (!config_.format.is_valid() || config_.frame_count == 0
        || config_.network_queue_slots == 0 || config_.session_timeout <= std::chrono::milliseconds(0)
        || config_.session_reap_interval <= std::chrono::milliseconds(0)
        || config_.rpc_port == 0
        || !packetizer_.valid() || !frame_queue_.valid()
        || frame_bytes == 0
        || static_cast<std::size_t>(config_.frame_count)
            > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        stop();
        return false;
    }

    try {
        (void)::aqua::net::parse_ip_address(config_.advertised_udp_address);
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: invalid advertised UDP address '{}' - {}",
            config_.advertised_udp_address, e.what());
        stop();
        return false;
    }

    const auto payload_bytes = static_cast<std::size_t>(config_.frame_count) * frame_bytes;
    if (payload_bytes > config::UDP_AUDIO_PAYLOAD_BYTES) {
        log_error_fmt("ServerRuntime: AudioFrame payload {} exceeds UDP safe payload budget {}",
            payload_bytes, config::UDP_AUDIO_PAYLOAD_BYTES);
        stop();
        return false;
    }

    try {
        reap_state_ = std::make_shared<ReapState>(ioc_);
    } catch (const std::bad_alloc&) {
        log_error("ServerRuntime: failed to allocate session reaper state");
        stop();
        return false;
    }

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        stop();
        return false;
    }
    capture_ = audio::create_capture(*device_mgr_);
    if (!capture_) {
        stop();
        return false;
    }

    if (!udp_.bind(config_.udp_bind_ip, config_.udp_port)
        || !udp_.start()
        || !dispatcher_.start()) {
        stop();
        return false;
    }

    grpc_ = std::make_unique<grpc::GrpcServer>(
        *sessions_, config_.format, config_.frame_count,
        config_.rpc_bind_ip, config_.rpc_port,
        grpc::AdvertisedUdpEndpoint { config_.advertised_udp_address, udp_.local_endpoint().port() });
    if (!grpc_->is_started()) {
        stop();
        return false;
    }
    try {
        grpc_thread_ = std::thread([this] { grpc_->run(); });
    } catch (const std::exception& e) {
        log_error_fmt("ServerRuntime: failed to start gRPC worker thread: {}", e.what());
        stop();
        return false;
    } catch (...) {
        log_error("ServerRuntime: failed to start gRPC worker thread");
        stop();
        return false;
    }

    auto capture_cfg = config_.capture;
    capture_cfg.format = config_.format;
    if (!capture_->start(capture_cfg,
            [this](const audio::AudioBlock& block) noexcept { on_capture_block(block); },
            [this](audio::AudioError error) noexcept { on_capture_event(error); })) {
        stop();
        return false;
    }

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
            stop();
            return false;
        } catch (...) {
            log_error("ServerRuntime: failed to schedule session reaper");
            stop();
            return false;
        }
    }
    return state_.load(std::memory_order_acquire) == RuntimeState::Running
        || state_.load(std::memory_order_acquire) == RuntimeState::Degraded;
}

void ServerRuntime::stop() noexcept
{
    if (!enter_stopping()) {
        return;
    }

    if (capture_) {
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
    dispatcher_.stop();
    udp_.stop();
    if (grpc_) {
        grpc_->shutdown();
    }
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
    sessions_->clear();
    enter_stopped();
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
