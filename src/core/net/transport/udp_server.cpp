#include "core/net/transport/udp_server.h"

namespace aqua::net {

// 构造：仅创建共享的 State（strand + 绑定 strand 的 socket），
// 实际打开与绑定推迟到 bind()。
UdpServer::UdpServer(asio::io_context& ioc)
    : UdpSocketBase(ioc)
{
}

// 绑定固定端口。
// 请求 SO_REUSEADDR 以支持"进程异常退出后立即重启"场景（固定端口可能仍在
// TIME_WAIT / 残留绑定中）；是否真正设置由基类按平台决定——Windows 不设置，
// 见 udp_socket_base.cpp 中 open_and_bind 的注释。
bool UdpServer::bind(const std::string& bind_ip, std::uint16_t port)
{
    return open_and_bind(bind_ip, port, /*reuse_address=*/true);
}

} // namespace aqua::net
