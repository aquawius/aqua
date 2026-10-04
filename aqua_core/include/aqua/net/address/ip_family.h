#ifndef AQUA_NET_ADDRESS_IP_FAMILY_H
#define AQUA_NET_ADDRESS_IP_FAMILY_H

// 地址族判定的唯一入口：族归一化、呈现、以及 gRPC peer 文本解析。
//
// 为什么不直接用 asio::ip::address::is_v6()：
//   1. **IPv4-mapped IPv6（::ffff:a.b.c.d）在线程/套接字层面是 IPv6，但它服务的是
//      IPv4 客户端**。本项目把"客户端的族"当作数据面路由的键（每族一个 socket），
//      所以这类地址必须先归一化成 V4——否则会出现"用 v6 槽去服务 v4 客户端"，
//      也会让 session 的族与它实际用的 socket 不一致。
//   2. gRPC 的 ServerContext::peer() 是**文本形态**（ipv4:a.b.c.d:port /
//      ipv6:[..]:port），不能直接喂 parse_ip_address()。
//
// 分层约定：本模块只做"判定/呈现"，不做任何路由决策，也不碰 socket。
// 需要发送到某地址时，必须使用**原始** endpoint，不得用 normalize_for_display()
// 的结果去构造目标地址（见该函数的注释）。

#include <asio/ip/address.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aqua::net {

// 有效地址族。
// Unknown 有两个真实用途，都不是占位：
//   1. **会话已由 Connect 创建、但首个 UDP heartbeat 还没到**——此时族尚未被观测到
//      （族只能由"收到包的那个 socket"确定，见提案 §9.4/§9.5）。
//   2. 运算结果无法判定。
// Unknown 永远不参与路由：它不是任何 socket 的族，FamilySet::contains() 恒为 false。
enum class IpFamily : std::uint8_t {
    Unknown = 0,
    V4 = 4,
    V6 = 6,
};

// 日志/错误消息用的短名："v4" / "v6" / "unknown"。
[[nodiscard]] constexpr const char* ip_family_name(IpFamily family) noexcept
{
    switch (family) {
    case IpFamily::V4:
        return "v4";
    case IpFamily::V6:
        return "v6";
    default:
        return "unknown";
    }
}

// 归一化后的族：IPv4-mapped IPv6 → V4；其余按 asio 的判定。
[[nodiscard]] IpFamily effective_family(const asio::ip::address& address) noexcept;

// 呈现用：IPv4-mapped IPv6 还原成 v4 文本（"::ffff:1.2.3.4" → "1.2.3.4"），
// 其余原样 to_string()。
//
// ⚠️ **只用于日志 / 诊断 / UI**。绝不能用它的返回值替换"用于发送的存储 endpoint"：
// 在 AF_INET6 socket 上用 AF_INET 目标做 send_to 会直接 EAFNOSUPPORT。
// 需要同时满足"日志好看"和"发送正确"时，存储原始地址、只在打印时调用本函数。
[[nodiscard]] std::string normalize_for_display(const asio::ip::address& address);

// 一个实例"服务了哪些族"。用于表达数据面绑定结果 / 能力集，供拒绝判定与启动日志使用。
struct FamilySet {
    bool v4 { false };
    bool v6 { false };

    [[nodiscard]] constexpr bool contains(IpFamily family) const noexcept
    {
        return (family == IpFamily::V4 && v4) || (family == IpFamily::V6 && v6);
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return !v4 && !v6; }
    // "v4+v6" / "v4" / "v6" / "none"
    [[nodiscard]] std::string to_string() const
    {
        if (v4 && v6) {
            return "v4+v6";
        }
        if (v4) {
            return "v4";
        }
        if (v6) {
            return "v6";
        }
        return "none";
    }
};

// gRPC ServerContext::peer() 的解析结果。
struct GrpcPeer {
    IpFamily family { IpFamily::Unknown }; // 已归一化（::ffff: → V4）
    asio::ip::address address { };
    std::uint16_t port { 0 };
};

// 解析 gRPC peer 文本。已知形态：
//   "ipv4:1.2.3.4:5678"             → family = V4
//   "ipv6:[::1]:5678"              → family = V6
//   "ipv6:[::ffff:1.2.3.4]:5678"   → 归一化为 V4
// 无法判定（unix: / unix-abstract: / 空串 / 端口非法 / 地址无法解析）返回 nullopt。
//
// ⚠️ 调用方（Connect 期的防御性拒绝）**必须把 nullopt 当作"未知"而不是"不匹配"**：
// 解析失败可能导致误拒合法客户端，宁可漏判——客户端侧还有一道族自证（见提案 §9.7）。
[[nodiscard]] std::optional<GrpcPeer> parse_grpc_peer_address(std::string_view peer);

} // namespace aqua::net

#endif // AQUA_NET_ADDRESS_IP_FAMILY_H
