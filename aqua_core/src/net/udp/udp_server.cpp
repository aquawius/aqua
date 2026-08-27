#include "aqua/net/udp/udp_server.h"

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
}

UdpServer::~UdpServer()
{
    stop();
}

bool UdpServer::bind(const std::string& bind_ip, std::uint16_t port)
{
    return state_->transport->bind(bind_ip, port);
}

bool UdpServer::start()
{
    const auto st = state_;
    return st->transport->start_receive(
        [st](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            const auto frame = NetworkFrame::decode(data);
            if (!frame || frame->type() != PacketType::Hello) {
                return;
            }
            if (!st->sessions->establish_session(frame->session_id(), sender)) {
                return;
            }
            const auto ack = NetworkFrame::hello_ack(frame->session_id()).encode();
            st->transport->send_to(sender, ack);
        });
}

void UdpServer::stop() noexcept
{
    state_->transport->stop();
}

std::size_t UdpServer::broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept
{
    if (!datagram || datagram->empty()) {
        return 0;
    }

    try {
        state_->sessions->snapshot_connected(state_->connected_scratch);
        const auto count = state_->connected_scratch.size();
        for (const auto& session : state_->connected_scratch) {
            state_->transport->send_to_shared(session.endpoint, datagram);
        }
        return count;
    } catch (...) {
        // Network worker boundary: one allocation/locking failure only drops this
        // broadcast, never escapes into the dispatcher thread.
        return 0;
    }
}

UdpTransportStats UdpServer::stats() const noexcept
{
    return state_->transport->stats();
}

asio::ip::udp::endpoint UdpServer::local_endpoint() const noexcept
{
    return state_->transport->socket_local_endpoint();
}

} // namespace aqua::net
