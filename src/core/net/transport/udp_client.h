#ifndef AQUA_UDP_CLIENT_H
#define AQUA_UDP_CLIENT_H

// UDP 客户端传输层：打开本地临时端口，与远端（通常为 server）通信。
//
// 与 UdpServer 的区别：不绑定固定端口（绑定 OS 分配的临时端口），
// 并维护一个"默认发送目标"（set_remote），send()/send_shared() 免参发送。
// 不调用 UDP socket::connect()（不建立内核过滤/错误上报语义），
// session/peer 的归属由 Aqua 上层协议按收包来源 endpoint 自行判定。
//
// 典型用法：
//   UdpClient client(ioc);
//   client.set_remote(server_ip, udp_port); // 记录默认发送目标（内部自动 open）
//   client.start_receive(handler);          // 接收 server 的 ACK / 音频
//   client.send(hello_bytes);               // 发给默认目标
//
// 停止后不可复用（见 UdpSocketBase::stop），重连请新建实例。

#include "core/net/transport/udp_socket_base.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace aqua::net {

// UDP 客户端传输层：打开临时本地端口，并记录一个默认远端 endpoint。
// 不调用 UDP socket::connect()；session/peer 语义由 Aqua 上层协议负责。
class UdpClient : public UdpSocketBase {
public:
    // 创建 client transport（仅创建 strand + socket，不打开；打开在
    // open()/set_remote() 中完成）。
    explicit UdpClient(asio::io_context& ioc);

    // 打开并绑定 0.0.0.0:0（由 OS 分配临时本地端口）。
    // 幂等：已打开则直接返回 true；失败（如实例已 stop）返回 false。
    bool open();

    // 设置默认发送目标（后续 send()/send_shared() 免参发送的对象）。
    // 未打开时自动 open()。远端端口为 0 时拒绝（非法目标）。
    // 失败（打开失败 / 地址非法）返回 false。
    bool set_remote(const asio::ip::udp::endpoint& remote);
    bool set_remote(const std::string& server_ip, std::uint16_t port);

    // 是否已设置默认发送目标（线程安全）。
    bool has_remote() const noexcept;
    // 获取默认发送目标（线程安全；未设置时返回端口为 0 的 endpoint）。
    asio::ip::udp::endpoint remote_endpoint() const noexcept;

    // 便捷发送：发送给 set_remote() 指定的 endpoint（拷贝语义，见基类 send_copy）。
    // 未设置远端时丢弃并记录 debug（高频音频路径下不应因误用刷 error 日志）。
    void send(std::span<const std::byte> data);
    void send_shared(std::shared_ptr<const std::vector<std::byte>> data);

private:
    // remote_ 可被任意线程读写：set_remote() 在控制线程写，send() 在音频/
    // 业务线程读，用互斥保护；与基类 strand 上的发送队列解耦。
    mutable std::mutex remote_mutex_;
    asio::ip::udp::endpoint remote_ { };
};

} // namespace aqua::net

#endif // AQUA_UDP_CLIENT_H
