#ifndef AQUA_UDP_TRANSPORT_H
#define AQUA_UDP_TRANSPORT_H

#include "core/public/config.h"

#include <asio.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace aqua::net {

// UDP 数据面传输层封装。
// 基于 asio::io_context 异步收发，回调在 io_context 线程触发。
// 不持有 SessionManager 引用；收到包后通过回调上交，由上层做路由。
class UdpTransport {
public:
    using ReceiveHandler = std::function<void(
        const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data)>;

    explicit UdpTransport(asio::io_context& ioc);
    ~UdpTransport();

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // 绑定本地端口。bind_ip 为 "0.0.0.0" 表示监听所有接口。
    // 返回 false 表示绑定失败（端口被占用等）。
    bool bind(const std::string& bind_ip, std::uint16_t port);

    // 启动异步接收循环。handler 在 io_context 线程触发，禁止阻塞。
    void start_receive(ReceiveHandler handler);

    // 异步发送数据到目标 endpoint。
    void send(const asio::ip::udp::endpoint& target,
              std::span<const std::byte> data);

    // 停止收发并关闭 socket。
    void stop();

    // 是否已绑定并运行。
    bool is_open() const noexcept;

    // 获取实际绑定的本地 endpoint（用于 bind 端口=0 后查询真实端口）。
    asio::ip::udp::endpoint socket_local_endpoint() const;

private:
    void do_receive();

    asio::io_context& ioc_;
    asio::ip::udp::socket socket_;
    ReceiveHandler handler_;

    // 预分配接收缓冲，避免在回调中分配堆内存。
    // 覆盖最大 UDP datagram，支持大于 MTU 的音频包（IP 分片重组后）。
    static constexpr std::size_t RECV_BUF_SIZE = aqua::config::UDP_RECV_BUF_SIZE;
    std::array<std::byte, RECV_BUF_SIZE> recv_buf_{};
    asio::ip::udp::endpoint recv_endpoint_{};
};

} // namespace aqua::net

#endif // AQUA_UDP_TRANSPORT_H
