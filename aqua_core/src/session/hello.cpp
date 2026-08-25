#include "aqua/session/hello.h"

#include "aqua/net/udp/udp_packet.h"

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
    if (net::decode_packet_type(datagram) != net::PacketType::Hello) {
        return false;
    }
    std::uint32_t session_id = 0;
    if (!net::decode_hello_packet(datagram, session_id)) {
        return false;
    }
    if (!sessions_.establish_session(session_id, sender)) {
        return false; // 未知 session 或非法 endpoint
    }
    if (send_ack_ != nullptr) {
        const auto ack = net::encode_hello_ack_packet(session_id);
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
        const auto hello = net::encode_hello_packet(session_id_);
        send_packet_(ud_, hello);
    }
}

} // namespace aqua
