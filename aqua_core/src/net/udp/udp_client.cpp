#include "aqua/net/udp/udp_client.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <utility>

namespace aqua::net {

// 构造：仅创建共享的 State（strand + socket），打开与设置远端由
// open()/set_remote() 完成。open() 默认打开 IPv4；IPv6 请直接使用 set_remote(ipv6, port)，
// 它会根据远端地址族选择正确的 socket。
UdpClient::UdpClient(asio::io_context& ioc)
    : UdpSocketBase(ioc)
{
}

// 打开并绑定 OS 分配的临时本地端口。
// 客户端不需要 SO_REUSEADDR：绑定的是临时端口，不存在固定端口复用冲突。
bool UdpClient::open()
{
    if (is_open()) {
        return true; // 幂等：已打开直接成功
    }
    return open_and_bind("0.0.0.0", 0, false);
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
        // 未打开时根据远端地址族选择 socket：IPv4 绑定 0.0.0.0:0，
        // IPv6 绑定 :::0。打开 socket 后地址族不可再切换，因此必须在
        // 第一次 set_remote() 时决定。
        const char* bind_ip = remote.address().is_v6() ? "::" : "0.0.0.0";
        if (!open_and_bind(bind_ip, 0, false)) {
            return false;
        }
    } else if (socket_local_endpoint().address().is_v4() != remote.address().is_v4()) {
        log_error("UdpClient::set_remote rejected: remote address family differs from open socket");
        return false;
    }
    {
        // 加锁写入：send() 线程可能正在 remote_endpoint() 读它。
        std::lock_guard lock(remote_mutex_);
        remote_ = remote;
    }
    return true;
}

// 设置默认发送目标（字符串版）：解析 IP 字面量（不支持 DNS 主机名）。IPv6
// 地址可带方括号，例如 [2001:db8::1]；本函数会自动选择 IPv6 socket。
bool UdpClient::set_remote(const std::string& server_ip, std::uint16_t port)
{
    if (port == 0) {
        log_error_fmt("UdpClient::set_remote rejected: remote port is 0 for {}", server_ip);
        return false;
    }
    try {
        return set_remote(asio::ip::udp::endpoint(::aqua::net::parse_ip_address(server_ip), port));
    } catch (const std::exception& e) {
        // make_address 对非法 IP 字面量抛异常，转为返回 false 并记录。
        log_error_fmt("UdpClient set_remote failed: invalid address {}:{} - {}",
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
