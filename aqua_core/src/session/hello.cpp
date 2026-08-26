#include "aqua/session/hello.h"

#include "aqua/net/udp/network_frame.h"

namespace aqua {

HelloResponder::HelloResponder(SessionManager& sessions, AckSender send_ack, void* ud)
    : sessions_(sessions)
    , send_ack_(send_ack)
    , ud_(ud)
{
}

bool HelloResponder::handle(const asio::ip::udp::endpoint& sender,
    std::span<const std::byte> datagram) noexcept
{
    const auto frame = net::NetworkFrame::decode(datagram);
    if (!frame || frame->type() != net::PacketType::Hello) {
        return false;
    }
    const std::uint32_t session_id = frame->session_id();
    if (!sessions_.establish_session(session_id, sender)) {
        return false; // 未知 session 或非法 endpoint
    }
    if (send_ack_ != nullptr) {
        const auto ack = net::NetworkFrame::hello_ack(session_id).encode();
        send_ack_(ud_, sender, ack);
    }
    return true;
}

HelloSender::HelloSender(std::uint32_t session_id, PacketSender send_packet, void* ud)
    : session_id_(session_id)
    , send_packet_(send_packet)
    , ud_(ud)
{
}

void HelloSender::send() noexcept
{
    if (send_packet_ != nullptr) {
        const auto hello = net::NetworkFrame::hello(session_id_).encode();
        send_packet_(ud_, hello);
    }
}

} // namespace aqua
