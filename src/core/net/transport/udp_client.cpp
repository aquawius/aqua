#include "core/net/transport/udp_client.h"

#include "core/logger/logger.h"
#include "core/net/address_utils.h"

#include <utility>

namespace aqua::net {

// 构造：仅创建共享的 State（strand + socket），打开与设置远端由
// open()/set_remote() 完成。
UdpClient::UdpClient(asio::io_context& ioc)
    : UdpSocketBase(ioc)
{
}

// 打开并绑定 OS 分配的临时本地端口。
// 客户端不需要 SO_REUSEADDR：绑定的是临时端口，不存在固定端口复用冲突。
bool UdpClient::open()
{
    if (is_open()) {
        return true;
    }
    return open_local(asio::ip::udp::v4());
}

// 设置默认发送目标（endpoint 版）。先校验端口非 0，再确保 socket 已打开。
bool UdpClient::set_remote(const asio::ip::udp::endpoint& remote)
{
    // 端口 0 不是合法对端（0 表示"未指定/通配"），直接拒绝，避免后续
    // send 把数据发往无效目标。
    if (remote.port() == 0) {
        log_error("UdpClient::set_remote rejected: remote port is 0");
        return false;
    }
    if (!is_open()) {
        if (!open_local(remote.protocol())) {
            return false; // 打开失败（或地址族初始化失败）则无法发送
        }
    } else if (socket_local_endpoint().protocol() != remote.protocol()) {
        // 已打开的 socket 地址族不能在运行中切换；否则 IPv4 socket 无法直接
        // 向 IPv6 endpoint 发送，反之亦然。必须创建新的 UdpClient 实例。
        log_error("UdpClient::set_remote rejected: address family does not match open socket");
        return false;
    }
    {
        // 加锁写入：send() 线程可能正在 remote_endpoint() 读它。
        std::lock_guard lock(remote_mutex_);
        remote_ = remote;
    }
    return true;
}

// 设置默认发送目标（字符串版）：解析 IP 字面量（不支持 DNS 主机名，与
// 基类 open_and_bind 的 make_address 行为一致）。
bool UdpClient::set_remote(const std::string& server_ip, std::uint16_t port)
{
    if (port == 0) {
        log_error_fmt("UdpClient::set_remote rejected: remote port is 0 for {}", server_ip);
        return false;
    }
    try {
        return set_remote(asio::ip::udp::endpoint(parse_ip_address(server_ip), port));
    } catch (const std::exception& e) {
        // make_address 对非法 IP 字面量抛异常，转为返回 false 并记录。
        log_error_fmt("UdpClient set_remote failed: invalid IP address {}:{} - {}",
            server_ip, port, e.what());
        return false;
    }
}

// 是否已设置默认发送目标：端口 0 表示未设置。
bool UdpClient::has_remote() const noexcept
{
    std::lock_guard lock(remote_mutex_);
    return remote_.port() != 0;
}

// 获取默认发送目标（锁内拷贝，返回快照，不暴露内部引用）。
asio::ip::udp::endpoint UdpClient::remote_endpoint() const noexcept
{
    std::lock_guard lock(remote_mutex_);
    return remote_;
}

// 便捷发送：先取默认目标快照（锁内拷贝，避免持锁调用基类发送），
// 再走基类 send_copy 的拷贝语义入队。
void UdpClient::send(std::span<const std::byte> data)
{
    const auto remote = remote_endpoint();
    if (remote.port() == 0) {
        log_debug("UdpClient::send ignored: remote endpoint not set");
        return;
    }
    UdpSocketBase::send_copy(remote, data);
}

// 便捷发送（共享缓冲版）：同上，走基类 send_shared 的零拷贝队列路径。
void UdpClient::send_shared(std::shared_ptr<const std::vector<std::byte>> data)
{
    const auto remote = remote_endpoint();
    if (remote.port() == 0) {
        log_debug("UdpClient::send_shared ignored: remote endpoint not set");
        return;
    }
    UdpSocketBase::send_shared(remote, std::move(data));
}

} // namespace aqua::net
