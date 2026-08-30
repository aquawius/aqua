#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
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
    log_debug("UdpClient created");
}

UdpClient::~UdpClient()
{
    stop();
}

bool UdpClient::set_remote(const std::string& server_ip, std::uint16_t port)
{
    const auto st = state_;
    if (st->receive_started.load(std::memory_order_acquire)
        || st->hello_started.load(std::memory_order_acquire)) {
        log_warn("UdpClient::set_remote ignored after data-plane startup");
        return false;
    }
    return st->transport->set_remote(server_ip, port);
}

bool UdpClient::start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame)
{
    if (expected_payload_bytes == 0 || !on_frame) {
        log_error("UdpClient::start_receive rejected: payload size must be non-zero and handler must be set");
        return false;
    }
    const auto st = state_;

    // 在打开 socket 前先校验远端 endpoint。若先打开，即便本次调用即将失败，
    // 也会固定 socket 地址族（IPv4/IPv6），可能导致之后合法的 set_remote()
    // 选择到不兼容的地址族。
    const auto expected_sender = st->transport->remote_endpoint();
    if (expected_sender.port() == 0) {
        log_error("UdpClient::start_receive rejected: remote endpoint is not set");
        return false;
    }

    if (!st->transport->is_open() && !st->transport->open()) {
        return false;
    }

    // 一次接收循环期间接收配置不可变。把值捕获进 transport handler，
    // 而不是每收一个包都去读可变的 State 字段。
    bool expected = false;
    if (!st->receive_started.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        log_warn("UdpClient::start_receive called twice, ignoring");
        return false;
    }
    auto handler = std::make_shared<FrameHandler>(std::move(on_frame));
    const std::weak_ptr<State> weak_st = st;
    const auto local_endpoint = st->transport->local_endpoint();
    log_debug_fmt("UdpClient receive configuration: local={} expected_payload={} remote={}",
        format_host_port(local_endpoint.address().to_string(), local_endpoint.port()),
        expected_payload_bytes,
        format_host_port(expected_sender.address().to_string(), expected_sender.port()));
    log_debug_fmt("UdpClient attaching receive handler: expected_sender={}",
        format_host_port(expected_sender.address().to_string(), expected_sender.port()));
    const bool started = st->transport->start_receive(
        [weak_st, expected_sender, expected_payload_bytes, handler](
            const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) mutable {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
            if (sender != expected_sender) {
                st->unexpected_sender_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpClient ignored datagram from unexpected sender: {}",
                    format_host_port(sender.address().to_string(), sender.port()));
                return;
            }
            const auto frame = NetworkFrame::decode(data);
            if (!frame) {
                st->malformed_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpClient ignored malformed datagram: bytes={}", data.size());
                return;
            }
            if (frame->type() == PacketType::HelloAck) {
                if (frame->session_id() == st->hello_session_id.load(std::memory_order_acquire)
                    && frame->session_id() != 0) {
                    st->hello_ack_generation.fetch_add(1, std::memory_order_acq_rel);
                    st->hello_ack_count.fetch_add(1, std::memory_order_relaxed);
                    log_trace_fmt("UdpClient HELLO_ACK received: session=0x{:08X}", frame->session_id());
                    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    st->last_hello_ack_ms.store(now_ms, std::memory_order_release);
                } else {
                    st->wrong_session_acks.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            if (frame->type() != PacketType::Audio) {
                st->non_audio_datagrams.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (frame->payload().size() != expected_payload_bytes) {
                st->audio_payload_mismatches.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UdpClient: dropping audio seq={} with payload={} bytes, expected={}",
                    frame->sequence(), frame->payload().size(), expected_payload_bytes);
                return;
            }
            log_trace_fmt("UdpClient audio frame accepted: seq={} bytes={}",
                frame->sequence(), frame->payload().size());
            if (*handler) {
                (*handler)(frame->sequence(), frame->payload());
                st->audio_frames_accepted.fetch_add(1, std::memory_order_relaxed);
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
    bool expected_hello_started = false;
    if (!st->hello_started.compare_exchange_strong(expected_hello_started, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        log_warn("UdpClient::start_hello called twice, ignoring");
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
                const auto local_endpoint = st->transport->local_endpoint();
                const auto remote_endpoint = st->transport->remote_endpoint();
                log_debug_fmt("UdpClient HELLO configuration: session=0x{:08X} local={} remote={} interval={}ms ack_miss_threshold={}",
                    session_id,
                    format_host_port(local_endpoint.address().to_string(), local_endpoint.port()),
                    format_host_port(remote_endpoint.address().to_string(), remote_endpoint.port()),
                    interval.count(), config::HELLO_ACK_MISS_THRESHOLD);
                const auto hello = NetworkFrame::hello(
                    st->hello_session_id.load(std::memory_order_acquire)).encode();
                st->transport->send(hello);
                st->hello_send_attempts.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UdpClient initial HELLO sent: session=0x{:08X}", session_id);
                log_trace_fmt("UdpClient HELLO sent: session=0x{:08X}", session_id);
                schedule_hello(st);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: failed to start HELLO scheduler: {}", format_exception_message(e));
                st->hello_failed.store(true, std::memory_order_release);
                st->hello_stopped.store(true, std::memory_order_release);
                st->hello_timer.reset();
            } catch (...) {
                log_error("UdpClient: failed to start HELLO scheduler");
                st->hello_failed.store(true, std::memory_order_release);
                st->hello_stopped.store(true, std::memory_order_release);
                st->hello_timer.reset();
            }
        });
        return true;
    } catch (const std::exception& e) {
        st->hello_started.store(false, std::memory_order_release);
        log_error_fmt("UdpClient::start_hello failed to schedule: {}", format_exception_message(e));
        return false;
    } catch (...) {
        st->hello_started.store(false, std::memory_order_release);
        log_error("UdpClient::start_hello failed to schedule");
        return false;
    }
}


void UdpClient::stop() noexcept
{
    const auto st = state_;
    log_debug("UdpClient stop requested");
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
            log_debug("UdpClient HELLO scheduler stopped on strand");
        });
    } catch (...) {
        // 若已无法再 post，State 会保活定时器直到所有待处理 handler/引用消失；
        // transport 的 stop 仍照常执行。
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
                state->hello_ack_miss_events.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpClient HELLO_ACK miss: consecutive={}",
                    state->consecutive_hello_ack_misses);
            } else {
                state->hello_ack_generation_seen = state->hello_ack_generation.load(std::memory_order_acquire);
                state->consecutive_hello_ack_misses = 0;
                log_trace("UdpClient HELLO_ACK observed; liveness miss counter reset");
            }
            state->hello_ack_misses.store(state->consecutive_hello_ack_misses, std::memory_order_release);

            if (!state->liveness_failed
                && state->consecutive_hello_ack_misses >= config::HELLO_ACK_MISS_THRESHOLD) {
                state->liveness_failed = true;
                if (state->on_liveness_failure) {
                    try {
                        state->on_liveness_failure(state->consecutive_hello_ack_misses);
                    } catch (const std::exception& e) {
                        log_error_fmt("UdpClient liveness handler exception: {}", format_exception_message(e));
                    } catch (...) {
                        log_error("UdpClient liveness handler unknown exception");
                    }
                }
            }

            try {
                const auto hello = NetworkFrame::hello(
                    state->hello_session_id.load(std::memory_order_acquire)).encode();
                state->transport->send(hello);
                log_trace_fmt("UdpClient HELLO sent: session=0x{:08X}",
                    state->hello_session_id.load(std::memory_order_relaxed));
                schedule_hello(state);
            } catch (const std::exception& e) {
                log_error_fmt("UdpClient: HELLO scheduling failed: {}", format_exception_message(e));
                state->hello_failed.store(true, std::memory_order_release);
                state->hello_stopped.store(true, std::memory_order_release);
                state->hello_timer.reset();
            } catch (...) {
                log_error("UdpClient: HELLO scheduling failed");
                state->hello_failed.store(true, std::memory_order_release);
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

bool UdpClient::hello_failed() const noexcept
{
    return state_->hello_failed.load(std::memory_order_acquire);
}

std::uint64_t UdpClient::audio_frames_accepted() const noexcept { return state_->audio_frames_accepted.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::malformed_datagrams() const noexcept { return state_->malformed_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::unexpected_sender_datagrams() const noexcept { return state_->unexpected_sender_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::wrong_session_acks() const noexcept { return state_->wrong_session_acks.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::audio_payload_mismatches() const noexcept { return state_->audio_payload_mismatches.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::non_audio_datagrams() const noexcept { return state_->non_audio_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::hello_send_attempts() const noexcept { return state_->hello_send_attempts.load(std::memory_order_relaxed); }
std::uint64_t UdpClient::hello_ack_miss_events() const noexcept { return state_->hello_ack_miss_events.load(std::memory_order_relaxed); }

} // namespace aqua::net
