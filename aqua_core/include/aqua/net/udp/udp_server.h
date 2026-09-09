#ifndef AQUA_NET_UDP_UDP_SERVER_H
#define AQUA_NET_UDP_UDP_SERVER_H

// UDP server 协议端点。
//
// 职责：
//   - 接收 heartbeat，校验 session id：首包建立 association（Created→Connected），
//     之后只刷新 NAT endpoint（漫游续命，不碰 last_seen）；每个合法 heartbeat
//     都回 HeartbeatAck（首包是建连确认，之后是路径探活回执）；
//   - 把已编码的 datagram 广播给所有 Connected session。
//
// audio 域类型不跨越此边界：AudioNetworkDispatcher 在调用 broadcast() 前
// 完成 AudioFrame -> NetworkFrame 的编码。

#include "aqua/net/udp/udp_transport.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

    // 广播同一份已编码 datagram；非实时线程调用。实现要求同一时刻只有一个
    // dispatcher/owner 调用方，因为内部复用 connected_scratch_。
    [[nodiscard]] std::optional<std::size_t> broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept;

    [[nodiscard]] UdpTransportStats stats() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_received() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_rejected() const noexcept;
    // 建连/续命计数不由本层重复记录：唯一真相源是 SessionManager 的
    // connected_/refreshed_，此处只做透传（与 session_stats() 同源，
    // GUI 读哪边都一致）。
    [[nodiscard]] std::uint64_t sessions_established() const noexcept;
    [[nodiscard]] std::uint64_t sessions_refreshed() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_ack_attempts() const noexcept;
    [[nodiscard]] std::uint64_t malformed_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t non_heartbeat_datagrams() const noexcept;

private:
    struct State {
        State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess);
        std::shared_ptr<UdpTransport> transport;
        std::shared_ptr<session::SessionManager> sessions;
        std::atomic<std::uint64_t> heartbeat_received { 0 };
        std::atomic<std::uint64_t> heartbeat_rejected { 0 };
        std::atomic<std::uint64_t> heartbeat_ack_attempts { 0 };
        std::atomic<std::uint64_t> malformed_datagrams { 0 };
        std::atomic<std::uint64_t> non_heartbeat_datagrams { 0 };
    };

    std::shared_ptr<State> state_;
    // broadcast() 有意绑定到 AudioNetworkDispatcher 的单一 worker 线程。
    // 复用这块临时存储，避免每个 AudioFrame 都做一次 vector 分配。
    std::vector<session::SessionManager::ConnectedSession> connected_scratch_;
};

} // namespace aqua::net

#endif // AQUA_NET_UDP_UDP_SERVER_H
