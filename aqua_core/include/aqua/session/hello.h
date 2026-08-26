#ifndef AQUA_SESSION_HELLO_H
#define AQUA_SESSION_HELLO_H

// HELLO 握手（数据面建连 / NAT 保活）：
//   - client 周期发 HELLO(session_id) → server 学习其 NAT 映射 endpoint 并回 HelloAck；
//   - 同时刷新 NAT 映射与 server session 的 last_seen（见 SessionManager::establish_session）。
// wire 布局见 network_frame.h（Hello/HelloAck 携带 4 字节 session_id）。

#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace aqua {

// 服务端：处理 HELLO → establish_session（学习 NAT endpoint）→ 回 HelloAck。
class HelloResponder {
public:
    using AckSender = void (*)(void* ud, const asio::ip::udp::endpoint& target,
        std::span<const std::byte> ack) noexcept;

    // sessions：引用不拥有；send_ack：回 HelloAck 的发送回调（由 runtime 绑定到 UdpServer）。
    HelloResponder(SessionManager& sessions, AckSender send_ack, void* ud);

    // 处理一个 datagram。type==Hello、解码成功且 establish 成功 → 回 ack 并返回 true；
    // 否则（非 Hello / malformed / 未知 session）返回 false 且不回复。
    bool handle(const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> datagram) noexcept;

private:
    SessionManager& sessions_;
    AckSender send_ack_;
    void* ud_;
};

// 客户端：发送 HELLO（由 runtime 用定时器周期驱动；本类只负责发一次）。
class HelloSender {
public:
    using PacketSender = void (*)(void* ud, std::span<const std::byte> packet) noexcept;

    // session_id 来自 ConnectResponse。
    HelloSender(std::uint32_t session_id, PacketSender send_packet, void* ud);

    void send() noexcept;

private:
    std::uint32_t session_id_;
    PacketSender send_packet_;
    void* ud_;
};

} // namespace aqua

#endif // AQUA_SESSION_HELLO_H
