#include "aqua/net/udp/udp_server.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/udp/network_frame.h"

#include <utility>

namespace aqua::net {

UdpServer::State::State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess)
    : transport(std::make_shared<UdpTransport>(ioc))
    , sessions(std::move(sess))
{
}

UdpServer::UdpServer(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sessions)
    : state_(std::make_shared<State>(ioc, std::move(sessions)))
{
    log_debug("UdpServer created");
}

UdpServer::~UdpServer()
{
    stop();
}

bool UdpServer::bind(const std::string& bind_ip, std::uint16_t port)
{
    log_debug_fmt("UdpServer bind requested: {}:{}", bind_ip, port);
    return state_->transport->bind(bind_ip, port);
}

bool UdpServer::start()
{
    const auto st = state_;
    log_debug("UdpServer starting receive/control handler");
    const std::weak_ptr<State> weak_st = st;
    const auto local = st->transport->local_endpoint();
    log_debug_fmt("UdpServer receive configuration: local={}",
        format_host_port(local.address().to_string(), local.port()));
    const bool started = st->transport->start_receive(
        [weak_st](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
            const auto frame = NetworkFrame::decode(data);
            if (!frame) {
                st->malformed_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpServer ignored malformed datagram: bytes={}", data.size());
                return;
            }
            if (frame->type() != PacketType::Hello) {
                st->non_hello_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpServer ignored non-HELLO datagram: bytes={}", data.size());
                return;
            }
            st->hello_received.fetch_add(1, std::memory_order_relaxed);
            log_trace_fmt("UdpServer HELLO received: session=0x{:08X} sender={} bytes={}",
                frame->session_id(), sender.address().to_string(), data.size());
            const bool was_connected = st->sessions->is_connected(frame->session_id());
            if (!st->sessions->establish_session(frame->session_id(), sender)) {
                st->hello_rejected.fetch_add(1, std::memory_order_relaxed);
                log_debug_fmt("UDP HELLO rejected: session=0x{:08X} sender={}",
                    frame->session_id(), sender.address().to_string());
                return;
            }
            if (was_connected) {
                st->sessions_refreshed.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UDP HELLO refreshed: session=0x{:08X} sender={}",
                    frame->session_id(), sender.address().to_string());
            } else {
                st->sessions_established.fetch_add(1, std::memory_order_relaxed);
                log_info_fmt("UDP session established: session=0x{:08X} sender={}",
                    frame->session_id(), sender.address().to_string());
            }
            const auto ack = NetworkFrame::hello_ack(frame->session_id()).encode();
            st->transport->send_to(sender, ack);
            st->hello_ack_attempts.fetch_add(1, std::memory_order_relaxed);
            log_trace_fmt("UDP HELLO_ACK queued: session=0x{:08X}", frame->session_id());
        });
    log_debug_fmt("UdpServer receive loop {}", started ? "started" : "failed to start");
    return started;
}

void UdpServer::stop() noexcept
{
    log_debug("UdpServer stop requested");
    state_->transport->stop();
}

std::optional<std::size_t> UdpServer::broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept
{
    if (!datagram || datagram->empty()) {
        return std::nullopt;
    }

    try {
        state_->sessions->snapshot_connected(connected_scratch_);
        for (const auto& session : connected_scratch_) {
            state_->transport->send_to_shared(session.endpoint, datagram);
        }
        return connected_scratch_.size();
    } catch (const std::exception& e) {
        log_warn_fmt("UdpServer::broadcast failed: {}", format_exception_message(e));
        return std::nullopt;
    } catch (...) {
        log_warn("UdpServer::broadcast failed: unknown exception");
        return std::nullopt;
    }
}

UdpTransportStats UdpServer::stats() const noexcept
{
    return state_->transport->stats();
}

asio::ip::udp::endpoint UdpServer::local_endpoint() const noexcept
{
    return state_->transport->local_endpoint();
}

std::uint64_t UdpServer::hello_received() const noexcept { return state_->hello_received.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::hello_rejected() const noexcept { return state_->hello_rejected.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::sessions_established() const noexcept { return state_->sessions_established.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::sessions_refreshed() const noexcept { return state_->sessions_refreshed.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::hello_ack_attempts() const noexcept { return state_->hello_ack_attempts.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::malformed_datagrams() const noexcept { return state_->malformed_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::non_hello_datagrams() const noexcept { return state_->non_hello_datagrams.load(std::memory_order_relaxed); }

} // namespace aqua::net
