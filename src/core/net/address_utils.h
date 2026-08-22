#ifndef AQUA_NET_ADDRESS_UTILS_H
#define AQUA_NET_ADDRESS_UTILS_H

// IP 地址辅助函数：只处理“字面量 IP + 端口”，不做 DNS 解析。
// 该层同时服务 UDP transport 与 gRPC：两者对 IPv6 的字符串表示规则不同，
// 例如 gRPC target 必须写成 [2001:db8::1]:50051，而 UDP endpoint 使用 address 对象。

#include <asio.hpp>

#include <cstdint>
#include <string>

namespace aqua::net {

// 解析 IP 字面量。允许 IPv4/IPv6 原始写法，也允许常见的方括号 IPv6 写法：
//   2001:db8::1
//   [2001:db8::1]
// 方括号只属于 host:port 字符串表示，不属于 asio::ip::address 的文本格式。
[[nodiscard]] inline asio::ip::address parse_ip_address(std::string host)
{
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    return asio::ip::make_address(host);
}

// 将 host + port 格式化成 gRPC / URI 风格的 host:port。
// IPv6 必须加方括号，IPv4 / 主机名则不加：
//   192.168.1.2:50051
//   [2001:db8::1]:50051
[[nodiscard]] inline std::string format_host_port(const std::string& host, std::uint16_t port)
{
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        return host + ":" + std::to_string(port);
    }
    if (host.find(':') != std::string::npos) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

} // namespace aqua::net

#endif // AQUA_NET_ADDRESS_UTILS_H
