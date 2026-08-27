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
    const std::weak_ptr<State> weak_st = st;
    return st->transport->start_receive(
        [weak_st](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            const auto st = weak_st.lock();
            if (!st) {
                return;
            }
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
    } catch (...) {
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

} // namespace aqua::net
