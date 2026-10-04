#include "aqua/net/address/ip_family.h"

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include <charconv>
#include <cstddef>
#include <system_error>

namespace aqua::net {
namespace {

// 把 IPv6 字面量里的 zone id 百分号编码还原：RFC 3986 里 '%' 必须写成 %25，
// 而 zone id 本身用的就是字面 '%'（如 fe80::1%eth0）。少数平台/版本的 gRPC
// 会输出编码形态。这是 M0 spike 未决的歧义点（见提案 §9.8），此处只做
// best-effort 兼容，不承诺覆盖全部形态。
std::optional<std::string> decode_percent_encoded_percent(std::string_view text)
{
    if (text.find("%25") == std::string_view::npos) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 3, "%25") == 0) {
            decoded.push_back('%');
            i += 3;
        } else {
            decoded.push_back(text[i]);
            ++i;
        }
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
    std::string_view rest;
    bool bracketed = false;
    if (peer.size() >= 5 && peer.compare(0, 5, "ipv4:") == 0) {
        rest = peer.substr(5);
    } else if (peer.size() >= 5 && peer.compare(0, 5, "ipv6:") == 0) {
        rest = peer.substr(5);
        bracketed = true;
    } else {
        // unix: / unix-abstract: / 空串 / 未知前缀：族不可判定。
        return std::nullopt;
    }

    // 端口永远是最后一个 ':' 之后的部分；IPv6 形态下地址已被方括号包住，
    // 地址内部的 ':' 不会干扰这个切分。
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= rest.size()) {
        return std::nullopt;
    }
    std::string_view address_text = rest.substr(0, colon);
    const auto port_text = rest.substr(colon + 1);

    if (bracketed) {
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
    auto address = asio::ip::make_address(std::string(address_text), ec);
    if (ec) {
        if (const auto decoded = decode_percent_encoded_percent(address_text)) {
            address = asio::ip::make_address(*decoded, ec);
        }
    }
    if (ec) {
        return std::nullopt;
    }

    return GrpcPeer { effective_family(address), address, static_cast<std::uint16_t>(port_value) };
}

} // namespace aqua::net
