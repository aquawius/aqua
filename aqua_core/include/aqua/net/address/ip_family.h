#ifndef AQUA_NET_ADDRESS_IP_FAMILY_H
#define AQUA_NET_ADDRESS_IP_FAMILY_H

// 地址族判定的唯一入口：族归一化、呈现、gRPC peer 文本解析，以及 zone（scope id）规则。
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
//      （族只能由"收到包的那个 socket"确定，见 doc/network_design.md §4.1）。
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

// 地址的**文本形态**是否属于该族的原生形态。
// 与 effective_family 的区别是关键：`::ffff:a.b.c.d` 归一化后是 V4，但它**不是**
// v4 的原生形态（文本是 v6）。凡是要把地址原样交给套接字层的地方（bind / 通告 /
// 数据面目标），必须用本函数而不是 effective_family——否则 v4 槽里的 v4-mapped
// 字面量会开出 AF_INET6 socket，正是"::ffff: 不进入系统"这条不变量要禁止的事。
[[nodiscard]] inline bool is_native_family(const asio::ip::address& address, IpFamily family) noexcept
{
    if (family == IpFamily::V4) {
        return address.is_v4();
    }
    if (family == IpFamily::V6) {
        return address.is_v6() && !address.to_v6().is_v4_mapped();
    }
    // Unknown 不是任何地址的"原生族"：与 FamilySet::contains(Unknown) 恒 false 同一条约定，
    // 避免把"族未知"当成"v6"（否则调用方漏判时会静默放行一个未校验的地址）。
    return false;
}

// 呈现用：IPv4-mapped IPv6 还原成 v4 文本（"::ffff:1.2.3.4" → "1.2.3.4"），
// 其余原样 to_string()。
//
// ⚠️ **只用于日志 / 诊断 / UI**。绝不能用它的返回值替换"用于发送的存储 endpoint"：
// 在 AF_INET6 socket 上用 AF_INET 目标做 send_to 会直接 EAFNOSUPPORT。
// 需要同时满足"日志好看"和"发送正确"时，存储原始地址、只在打印时调用本函数。
[[nodiscard]] std::string normalize_for_display(const asio::ip::address& address);

// link-local 的 IPv6 目标需要一个 **zone id**（scope id）才拨得通：它是逐主机本地的
// 接口索引，因此**不能**出现在 wire 上（INV-10），只能由对端自己补。
//
// 本函数实现设计文档承诺的"client 自己补 zone"：
//   - 目标不是 v6 link-local，或本来就带 zone  ⇒ 原样返回（无 error）；
//   - 目标是无 zone 的 v6 link-local          ⇒ 借用 reference（客户端的 `--rpc-ip`）的 zone。
//     控制面能连上就说明那块网卡面向这台 server，数据面复用同一个 scope 是对的；
//   - 两边都没有 zone                          ⇒ error，调用方必须**响亮失败**
//     （这就是"两端都缺 zone 就报明确错误"，而不是把包发向一个内核无法路由的目标）。
//
// ⚠️ zone 在 Windows 上只能是**十进制 ifIndex**：asio 的解析器（`socket_ops.ipp` 的
// Windows 分支）只接受数字，接口名形式仅在非 Windows 平台可用。因此补出来的一律是
// `%<数字>`，错误文案也写 `%<ifIndex>`。
struct LinkLocalTarget {
    // 需要时已补成 "fe80::1%24"；未变则原样返回。
    std::string address;
    // 非空 = 借不到 zone（或目标根本不是合法字面量），调用方应把它写进日志并失败。
    std::optional<std::string> error;
};

// target 必须是调用方已校验过的**原生**字面量；reference 是客户端的 `--rpc-ip`。
[[nodiscard]] LinkLocalTarget apply_link_local_scope(std::string_view target, std::string_view reference);

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

// 解析 gRPC peer 文本。**实测**（Windows gRPC 原样输出）：
//   "ipv4:1.2.3.4:5678"            → family = V4
//   "ipv6:%5B::1%5D:58305"         → family = V6（方括号是 percent-encode 的！）
//   "ipv6:%5B::ffff:1.2.3.4%5D:5"  → 归一化为 V4
// 字面的 "[::1]" 形态也接受（解析前统一做 percent-decode）。
// 无法判定（unix: / unix-abstract: / 空串 / 端口非法 / 地址无法解析）返回 nullopt。
//
// ⚠️ 调用方（Connect 期的族准入检查）**必须把 nullopt 当作"未知"而不是"不匹配"**：
// 解析失败可能导致误拒合法客户端，宁可漏判——客户端侧还有一道族自证（见 doc/network_design.md §3.2）。
// 也正因为这条规则，**解析器必须覆盖真实形态**：漏掉编码形态会让准入检查静默失效
// （实测踩过：peer_family=unknown ⇒ 不拒绝 ⇒ 退化成"连上了却收不到 ACK"）。
[[nodiscard]] std::optional<GrpcPeer> parse_grpc_peer_address(std::string_view peer);

} // namespace aqua::net

#endif // AQUA_NET_ADDRESS_IP_FAMILY_H
