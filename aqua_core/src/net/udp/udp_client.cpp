#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"

#include <algorithm>

namespace aqua::net {

UdpClient::State::State(asio::io_context& ioc)
    : ioc(ioc)
    , strand(asio::make_strand(ioc))
    , transport(std::make_shared<UdpTransport>(ioc))
{
}

UdpClient::UdpClient(asio::io_context& ioc)
    : state_(std::make_shared<State>(ioc))
{
}

UdpClient::~UdpClient()
{
    stop();
}

bool UdpClient::set_remote(const std::string& server_ip, std::uint16_t port)
{
    return state_->transport->set_remote(server_ip, port);
}

bool UdpClient::start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame)
{
    if (expected_payload_bytes == 0 || !on_frame) {
        log_error("UdpClient::start_receive rejected: payload size must be non-zero and handler must be set");
        return false;
    }
    const auto st = state_;

    // Validate the remote endpoint before opening the socket. Opening first would pin the
    // socket family (IPv4/IPv6) even though the call is going to fail, which could make a
    // later valid set_remote() choose an incompatible family.
    const auto expected_sender = st->transport->remote_endpoint();
    if (expected_sender.port() == 0) {
        log_error("UdpClient::start_receive rejected: remote endpoint is not set");
        return false;
    }

    if (!st->transport->is_open() && !st->transport->open()) {
        return false;
    }

    // Receive configuration is immutable for one receive loop. Capture the values into
    // the transport handler rather than consulting mutable State fields for every packet.
    bool expected = false;
    if (!st->receive_started.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        log_warn("UdpClient::start_receive called twice, ignoring");
        return false;
    }
    auto handler = std::make_shared<FrameHandler>(std::move(on_frame));
    const std::weak_ptr<State> weak_st = st;
    const bool started = st->transport->start_receive(
        [weak_st, expected_sender, expected_payload_bytes, handler](
            const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) mutable {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
            if (sender != expected_sender) {
                return;
            }
            const auto frame = NetworkFrame::decode(data);
            if (!frame) {
                return;
            }
            if (frame->type() == PacketType::HelloAck) {
                if (frame->session_id() == st->hello_session_id.load(std::memory_order_acquire)
                    && frame->session_id() != 0) {
                    st->hello_ack_generation.fetch_add(1, std::memory_order_acq_rel);
                    st->hello_ack_count.fetch_add(1, std::memory_order_relaxed);
                    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    st->last_hello_ack_ms.store(now_ms, std::memory_order_release);
                }
                return;
            }
            if (frame->type() != PacketType::Audio) {
                return;
            }
            if (frame->payload().size() != expected_payload_bytes) {
                log_debug_fmt("UdpClient: dropping audio seq={} with payload={} bytes, expected={}",
                    frame->sequence(), frame->payload().size(), expected_payload_bytes);
                return;
            }
            if (*handler) {
                (*handler)(frame->sequence(), frame->payload());
            }
        });
    if (!started) {
        st->receive_started.store(false, std::memory_order_release);
    }
    return started;
}

bool UdpClient::start_hello(std::uint32_t session_id, std::chrono::milliseconds interval,
    LivenessHandler on_liveness_failure)
{
    const auto st = state_;
    if (session_id == 0) {
        log_error("UdpClient::start_hello rejected: session_id is 0");
        return false;
    }
    if (interval <= std::chrono::milliseconds(0)) {
        log_error("UdpClient::start_hello rejected: interval must be > 0");
        return false;
    }
    if (st->hello_stopped.load(std::memory_order_acquire)) {
        return false;
    }
    if (!st->transport->has_remote()) {
        log_error("UdpClient::start_hello rejected: remote endpoint is not set");
        return false;
    }
    try {
        asio::post(st->strand, [st, session_id, interval,
            on_liveness_failure = std::move(on_liveness_failure)]() mutable {
            try {
                if (st->hello_stopped.load(std::memory_order_acquire)
                    || st->hello_timer != nullptr) {
                    return;
                }
                if (!st->transport->has_remote()) {
                    log_error("UdpClient::start_hello rejected: remote endpoint is not set");
                    return;
                }
                st->hello_session_id.store(session_id, std::memory_order_release);
                st->hello_interval = interval;
                st->hello_ack_generation_seen = st->hello_ack_generation.load(std::memory_order_acquire);
                st->consecutive_hello_ack_misses = 0;
                st->liveness_failed = false;
                st->on_liveness_failure = std::move(on_liveness_failure);
                st->hello_ack_misses.store(0, std::memory_order_release);
                st->last_hello_ack_ms.store(0, std::memory_order_release);
                st->hello_timer = std::make_unique<asio::steady_timer>(st->strand);
                const auto hello = NetworkFrame::hello(
                    st->hello_session_id.load(std::memory_order_acquire)).encode();
                st->transport->send(hello);
                schedule_hello(st);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: failed to start HELLO scheduler: {}", e.what());
                st->hello_stopped.store(true, std::memory_order_release);
                st->hello_timer.reset();
            } catch (...) {
                log_error("UdpClient: failed to start HELLO scheduler");
                st->hello_stopped.store(true, std::memory_order_release);
                st->hello_timer.reset();
            }
        });
        return true;
    } catch (const std::exception& e) {
        log_error_fmt("UdpClient::start_hello failed to schedule: {}", e.what());
        return false;
    } catch (...) {
        log_error("UdpClient::start_hello failed to schedule");
        return false;
    }
}


void UdpClient::stop() noexcept
{
    const auto st = state_;
    if (st->hello_stopped.exchange(true, std::memory_order_acq_rel)) {
        st->transport->stop();
        return;
    }
    try {
        asio::post(st->strand, [st] {
            if (st->hello_timer != nullptr) {
                asio::error_code ec;
                st->hello_timer->cancel(ec);
                st->hello_timer.reset();
            }
        });
    } catch (...) {
        // If posting is no longer possible, State keeps the timer alive until all
        // pending handlers/references are gone; transport stop is still performed.
    }
    st->transport->stop();
}

void UdpClient::schedule_hello(const std::shared_ptr<State>& state)
{
    if (state->hello_timer == nullptr
        || state->hello_stopped.load(std::memory_order_acquire)) {
        return;
    }
    state->hello_timer->expires_after(state->hello_interval);
    const std::weak_ptr<State> weak_state = state;
    state->hello_timer->async_wait(asio::bind_executor(state->strand,
        [weak_state](const asio::error_code& ec) {
            const auto state = weak_state.lock();
            if (!state || ec || state->hello_stopped.load(std::memory_order_acquire)) {
                return;
            }

            if (state->hello_ack_generation.load(std::memory_order_acquire)
                == state->hello_ack_generation_seen) {
                ++state->consecutive_hello_ack_misses;
            } else {
                state->hello_ack_generation_seen = state->hello_ack_generation.load(std::memory_order_acquire);
                state->consecutive_hello_ack_misses = 0;
            }
            state->hello_ack_misses.store(state->consecutive_hello_ack_misses, std::memory_order_release);

            if (!state->liveness_failed
                && state->consecutive_hello_ack_misses >= config::HELLO_ACK_MISS_THRESHOLD) {
                state->liveness_failed = true;
                if (state->on_liveness_failure) {
                    try {
                        state->on_liveness_failure(state->consecutive_hello_ack_misses);
                    } catch (const std::exception& e) {
                        log_error_fmt("UdpClient liveness handler exception: {}", e.what());
                    } catch (...) {
                        log_error("UdpClient liveness handler unknown exception");
                    }
                }
            }

            try {
                const auto hello = NetworkFrame::hello(
                    state->hello_session_id.load(std::memory_order_acquire)).encode();
                state->transport->send(hello);
                schedule_hello(state);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: HELLO scheduling failed: {}", e.what());
                state->hello_stopped.store(true, std::memory_order_release);
                state->hello_timer.reset();
            } catch (...) {
                log_error("UdpClient: HELLO scheduling failed");
                state->hello_stopped.store(true, std::memory_order_release);
                state->hello_timer.reset();
            }
        }));
}

bool UdpClient::has_remote() const noexcept
{
    return state_->transport->has_remote();
}

asio::ip::udp::endpoint UdpClient::remote_endpoint() const noexcept
{
    return state_->transport->remote_endpoint();
}

bool UdpClient::is_open() const noexcept
{
    return state_->transport->is_open();
}

asio::ip::udp::endpoint UdpClient::local_endpoint() const noexcept
{
    return state_->transport->local_endpoint();
}

UdpTransportStats UdpClient::stats() const noexcept
{
    return state_->transport->stats();
}

std::uint64_t UdpClient::hello_ack_count() const noexcept
{
    return state_->hello_ack_count.load(std::memory_order_relaxed);
}

std::uint32_t UdpClient::consecutive_hello_ack_misses() const noexcept
{
    return state_->hello_ack_misses.load(std::memory_order_acquire);
}

std::int64_t UdpClient::hello_ack_age_ms() const noexcept
{
    const auto last = state_->last_hello_ack_ms.load(std::memory_order_acquire);
    if (last == 0) {
        return -1;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return std::max<std::int64_t>(0, now - last);
}

} // namespace aqua::net
