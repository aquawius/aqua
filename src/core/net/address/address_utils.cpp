#include "core/net/address/address_utils.h"

#include <stdexcept>

namespace aqua::net {

// 将主机规范化为规范的IP字面量，然后格式化为 host:port。
// - IPv6 -> [地址]:端口（方括号用于区分冒号）
// - IPv4 -> 地址:端口
// 因为输入首先经过 parse_ip_address()，所以输出总是规范的地址字符串；
// 非IP输入（主机名）会在这里报错（见头文件）。
std::string format_host_port(const std::string& host, std::uint16_t port)
{
    const auto address = parse_ip_address(host);
    const std::string normalized = address.to_string();
    if (address.is_v6()) {
        return "[" + normalized + "]:" + std::to_string(port);
    }
    return normalized + ':' + std::to_string(port);
}

// 去掉可选的 IPv6 方括号（它们属于 host:port 格式，而不是 asio 地址文本格式），
// 然后用 asio 解析。空字符串会被明确拒绝，并给出比 make_address("") 更清晰的错误。
// 非 IP 输入会抛出 std::invalid_argument（出于设计原因不支持主机名，详见头文件）。
asio::ip::address parse_ip_address(const std::string& host)
{
    std::string value = host;
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        value = value.substr(1, value.size() - 2);
    }
    if (value.empty()) {
        throw std::invalid_argument("empty IP address");
    }
    return asio::ip::make_address(value);
}

} // namespace aqua::net
