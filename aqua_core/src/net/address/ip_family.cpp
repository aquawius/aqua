#include "aqua/net/address/ip_family.h"

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include <charconv>
#include <cstddef>
#include <system_error>

namespace aqua::net {
namespace {

// gRPC 的 peer 文本按 URI 规则 percent-encode（**实测**：IPv6 形态是
// "ipv6:%5B::1%5D:58305" —— 连方括号本身都被编码，不是字面的 "[::1]"）。
// 这里做通用解码：只认 "%" + 两位十六进制，非法序列原样保留（那更可能是
// zone id 里的字面 '%'）。解码 `%25` → `%` 也由这里统一处理。
std::string percent_decode(std::string_view text)
{
    if (text.find('%') == std::string_view::npos) {
        return std::string(text);
    }
    const auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        decoded.push_back(text[i]);
        ++i;
    }
    return decoded;
}

} // namespace

IpFamily effective_family(const asio::ip::address& address) noexcept
{
    // asio::ip::address 只可能是 v4 或 v6（没有第三种状态），因此这里不设"未知"
    // 分支——留一个恒不可达的分支只会骗过读者。
    if (!address.is_v6()) {
        return IpFamily::V4;
    }
    // ::ffff:a.b.c.d 在套接字层是 IPv6，但它服务的是 IPv4 客户端。
    return address.to_v6().is_v4_mapped() ? IpFamily::V4 : IpFamily::V6;
}

std::string normalize_for_display(const asio::ip::address& address)
{
    if (address.is_v6()) {
        const auto v6 = address.to_v6();
        if (v6.is_v4_mapped()) {
            return asio::ip::make_address_v4(asio::ip::v4_mapped, v6).to_string();
        }
    }
    return address.to_string();
}

std::optional<GrpcPeer> parse_grpc_peer_address(std::string_view peer)
{
    bool ipv6_form = false;
    if (peer.size() > 5 && peer.compare(0, 5, "ipv4:") == 0) {
        peer.remove_prefix(5);
    } else if (peer.size() > 5 && peer.compare(0, 5, "ipv6:") == 0) {
        peer.remove_prefix(5);
        ipv6_form = true;
    } else {
        // unix: / unix-abstract: / 空串 / 未知前缀：族不可判定。
        return std::nullopt;
    }

    // **整段解码一次**，再按结构解析。实测 gRPC 输出的是 URI 编码形态
    // （`ipv6:%5B::1%5D:58305`，方括号本身被编码），所以解码必须发生在结构解析之前。
    // 只解码一次：`%25` → `%` 是 zone 的正确编码（`fe80::1%2524` → `fe80::1%24`），
    // 二次解码会把它变成垃圾。顺带覆盖"连端口冒号也被编码"（`%3A`）的版本差异。
    // 注意：peer 里出现**字面**（未编码）的 `%` 属于 RFC 3986 违规形态，本函数不承诺支持
    // ——此时解析失败返回 nullopt，调用方按"未知"处理（fail-open，不误拒客户端）。
    const std::string decoded = percent_decode(peer);

    // 端口是**最后一个** ':' 之后的部分。IPv6 形态下地址被方括号包住，
    // 地址内部的 ':' 不会干扰这个切分。
    const auto colon = decoded.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= decoded.size()) {
        return std::nullopt;
    }
    std::string address_text = decoded.substr(0, colon);
    const auto port_text = std::string_view(decoded).substr(colon + 1);

    if (ipv6_form) {
        if (address_text.size() < 2 || address_text.front() != '[' || address_text.back() != ']') {
            return std::nullopt;
        }
        address_text = address_text.substr(1, address_text.size() - 2);
    }
    if (address_text.empty()) {
        return std::nullopt;
    }

    std::uint32_t port_value = 0;
    const auto* const port_begin = port_text.data();
    const auto* const port_end = port_begin + port_text.size();
    const auto parsed = std::from_chars(port_begin, port_end, port_value);
    if (parsed.ec != std::errc { } || parsed.ptr != port_end || port_value > 65535U) {
        return std::nullopt;
    }

    asio::error_code ec;
    const auto address = asio::ip::make_address(address_text, ec);
    if (ec) {
        return std::nullopt;
    }

    return GrpcPeer { effective_family(address), address, static_cast<std::uint16_t>(port_value) };
}

} // namespace aqua::net
