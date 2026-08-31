#ifndef AQUA_NET_ADDRESS_UTILS_H
#define AQUA_NET_ADDRESS_UTILS_H

#include <asio/ip/address.hpp>

#include <cstdint>
#include <string>

namespace aqua::net {

// 格式化用于日志和 gRPC 目标的主机/端口对。IPv6 字面量用方括号括起来，
// 这样地址/端口边界就不会模糊：[2001:db8::1]:50051。
//
// 本函数永不抛异常：host 不是合法 IP 字面量时退化为原始 host:port（用于日志/错误路径，
// 避免异常传播进 log 语句）。需要「检测非法地址」的调用方请先用 parse_ip_address() 显式校验。
//
// 重要说明：这个模块只接受 IP 字面量（IPv4 / IPv6，可选 IPv6 方括号）。
// 不支持主机名 / DNS 名——这是故意设计的，没有计划去实现它们。
// 需要使用主机名的调用方必须在调用前自行解析为 IP。
[[nodiscard]] std::string format_host_port(const std::string& host, std::uint16_t port);

// 解析 IP 字面量。IPv6 字面量可以用方括号括起来，以与 host:port 字符串保持一致。
// 这里刻意不解析 DNS 名称——和 format_host_port() 的决定一样：只支持 IP 字面量，
// 非 IP 输入会抛出 std::invalid_argument。
[[nodiscard]] asio::ip::address parse_ip_address(const std::string& host);

} // namespace aqua::net

#endif // AQUA_NET_ADDRESS_UTILS_H
