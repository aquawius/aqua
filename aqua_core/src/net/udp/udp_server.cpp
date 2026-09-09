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
    log_debug_fmt("UdpServer bind requested: {}", format_host_port(bind_ip, port));
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
            if (frame->type() == PacketType::Heartbeat) {
                // client→server 唯一包型：首包建立 association（Created→Connected），
                // 之后只刷新 endpoint（NAT 重绑/漫游时静默跟随，不碰 last_seen）。
                // 每个合法 heartbeat 都回 HeartbeatAck：首包是建连确认，之后是
                // 路径探活回执（client 两阶段都做 ACK 跟踪，无 ACK 即路径死亡）。
                // 未知 session 一律拒绝（不回 ACK）。
                st->heartbeat_received.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpServer heartbeat received: session=0x{:08X} sender={} bytes={}",
                    frame->session_id(), sender.address().to_string(), data.size());
                using aqua::session::SessionManager;
                switch (st->sessions->on_heartbeat(frame->session_id(), sender)) {
                case SessionManager::HeartbeatOutcome::Rejected:
                    st->heartbeat_rejected.fetch_add(1, std::memory_order_relaxed);
                    log_debug_fmt("UDP heartbeat rejected: session=0x{:08X} sender={}",
                        frame->session_id(), sender.address().to_string());
                    return;
                case SessionManager::HeartbeatOutcome::Refreshed:
                    st->sessions_refreshed.fetch_add(1, std::memory_order_relaxed);
                    log_trace_fmt("UDP heartbeat refreshed: session=0x{:08X} sender={}",
                        frame->session_id(), sender.address().to_string());
                    break; // 落到下方统一回 ACK（路径探活回执）
                case SessionManager::HeartbeatOutcome::Established:
                    st->sessions_established.fetch_add(1, std::memory_order_relaxed);
                    log_info_fmt("UDP session established: session=0x{:08X} sender={}",
                        frame->session_id(), sender.address().to_string());
                    break;
                }
                const auto ack = NetworkFrame::heartbeat_ack(frame->session_id()).encode();
                st->transport->send_to(sender, ack);
                st->heartbeat_ack_attempts.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UDP heartbeat ACK queued: session=0x{:08X}", frame->session_id());
                return;
            }
            st->non_heartbeat_datagrams.fetch_add(1, std::memory_order_relaxed);
            log_trace_fmt("UdpServer ignored non-heartbeat datagram: bytes={}", data.size());
            return;
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

std::uint64_t UdpServer::heartbeat_received() const noexcept { return state_->heartbeat_received.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::heartbeat_rejected() const noexcept { return state_->heartbeat_rejected.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::sessions_established() const noexcept { return state_->sessions_established.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::sessions_refreshed() const noexcept { return state_->sessions_refreshed.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::heartbeat_ack_attempts() const noexcept { return state_->heartbeat_ack_attempts.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::malformed_datagrams() const noexcept { return state_->malformed_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::non_heartbeat_datagrams() const noexcept { return state_->non_heartbeat_datagrams.load(std::memory_order_relaxed); }

} // namespace aqua::net
