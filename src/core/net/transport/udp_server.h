#ifndef AQUA_UDP_SERVER_H
#define AQUA_UDP_SERVER_H

// UDP 服务端传输层：绑定固定端口，接收任意客户端的数据报，并按 endpoint 定向回复。
//
// 典型用法：
//   UdpServer server(ioc);
//   server.bind("0.0.0.0", 9999);
//   server.start_receive([](const auto& sender, std::span<const std::byte> data) {
//       // 按 sender（NAT 映射后的客户端地址）路由：HELLO 握手、keepalive 刷新、
//       // 音频广播目标集合维护等，由上层 SessionManager 负责。
//   });
//   server.send_shared(client_ep, audio_payload); // 定向回复 / 音频广播
//
// 回调在 transport strand 上触发，禁止阻塞；数据 span 仅在回调内有效。

#include "core/net/transport/udp_socket_base.h"

#include <cstdint>
#include <string>

namespace aqua::net {

// UDP 服务端传输层：绑定固定端口，接收任意客户端的数据报，并按 endpoint 定向回复。
// 基于 asio::io_context 异步收发，回调在 io_context 线程触发。
// 不持有 SessionManager 引用；收到包后通过回调上交，由上层做路由。
class UdpServer : public UdpSocketBase {
public:
    // 创建 server transport（仅创建 strand + socket，不打开/不绑定，见
    // UdpSocketBase 说明；绑定在 bind() 中完成）。
    explicit UdpServer(asio::io_context& ioc);

    // 绑定本地端口。bind_ip 为 "0.0.0.0" 表示监听所有接口。
    // 返回 false 表示绑定失败（端口被占用、地址非法等），此时 socket 已关闭，
    // 修复问题后可重新调用 bind()（transport 未被 stop 的前提下）。
    bool bind(const std::string& bind_ip, std::uint16_t port);
};

} // namespace aqua::net

#endif // AQUA_UDP_SERVER_H
