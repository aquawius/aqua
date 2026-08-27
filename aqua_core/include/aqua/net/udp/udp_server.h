#ifndef AQUA_NET_UDP_UDP_SERVER_H
#define AQUA_NET_UDP_UDP_SERVER_H

// UDP server protocol endpoint.
//
// Responsibilities:
//   - receive HELLO, validate the session id and refresh the NAT endpoint;
//   - send HELLO_ACK;
//   - broadcast already-encoded datagrams to all Connected sessions.
//
// Audio domain types do not cross this boundary. AudioNetworkDispatcher performs
// AudioFrame -> NetworkFrame encoding before calling broadcast().

#include "aqua/net/udp/udp_transport.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aqua::net {

class UdpServer final {
public:
    UdpServer(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sessions);
    ~UdpServer();

    UdpServer(const UdpServer&) = delete;
    UdpServer& operator=(const UdpServer&) = delete;

    bool bind(const std::string& bind_ip, std::uint16_t port);
    bool start();
    void stop() noexcept;

    // 广播同一份已编码 datagram；只由单一 network worker 调用。
    // 返回本次看到的 Connected session 数量；0 表示当前没有接收者。
    std::size_t broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept;

    [[nodiscard]] UdpTransportStats stats() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;

private:
    struct State {
        State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess);
        std::shared_ptr<UdpTransport> transport;
        std::shared_ptr<session::SessionManager> sessions;
        std::vector<session::SessionManager::ConnectedSession> connected_scratch;
    };

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_NET_UDP_UDP_SERVER_H
