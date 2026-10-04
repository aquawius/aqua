#ifndef AQUA_NET_UDP_UDP_SERVER_H
#define AQUA_NET_UDP_UDP_SERVER_H

// UDP server 协议端点。
//
// 职责：
//   - 接收 heartbeat，校验 session id：首包建立 association（Created→Connected），
//     之后只刷新 NAT endpoint（漫游续命，不碰 last_seen）；每个合法 heartbeat
//     都回 HeartbeatAck（首包是建连确认，之后是路径探活回执）；
//   - 把已编码的 datagram 扇出给所有 Connected session。
//
// **数据面逐族**（提案 §9）：内部持有两条严格单族的 transport ——
//   v4: AF_INET socket；v6: AF_INET6 socket 且 v6_only=true。
// 两者共用同一端口（v6_only=true 是"同端口共存"能成立的前提），
// 各自一条接收循环。这样绑定 `0.0.0.0` 只接受原生 IPv4 来源、绑定 `::` 只接受
// 原生 IPv6 来源，`::ffff:` 表示法不会进入系统。
//
// 扇出按"会话 endpoint 所属族"路由到对应 transport。这不是优化而是**正确性**：
// 在 AF_INET6 socket 上用 AF_INET 目标 send_to 会 EAFNOSUPPORT；反过来，
// 换一条 socket 回包会打破 NAT 映射（映射绑定在 (本地端口, 远端, 族) 上）。
// endpoint 的唯一写入者是"收到它的那条 socket"，因此 endpoint 的族与 socket 的族
// 必然一致（见 SessionManager::on_heartbeat）。
//
// audio 域类型不跨越此边界：AudioNetworkDispatcher 在调用 broadcast() 前
// 完成 AudioFrame -> NetworkFrame 的编码。

#include "aqua/net/address/ip_family.h"
#include "aqua/net/udp/udp_transport.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aqua::net {

// 单个族的数据面绑定目标。
struct UdpBindTarget {
    // IP 字面量（IPv6 可带 zone id，如 fe80::1%24）。
    std::string address;
    // true = 用户显式指定（pin）。是否"失败即致命"由调用方据此判定（INV-5：
    // pin 严格、默认宽松）；本层只回报每族的成败，不替调用方做这个决策。
    bool required { false };
};

// 数据面绑定计划。nullopt = 该族数据面关闭（CLI 的 `none`）。
struct UdpBindPlan {
    std::optional<UdpBindTarget> v4;
    std::optional<UdpBindTarget> v6;
    // 两族共用的端口。**0 = 由 OS 分配临时端口**，本层允许（测试与单族用法依赖它：
    // 绑 0 之后用 local_endpoint() 取实际端口）。
    // ⚠️ server 运行时必须自己校验 > 0：临时端口一旦被通告给客户端就是错的
    // （这正是提案 §1.4 记的"Core 没有守住自己的不变量"缺口）。
    std::uint16_t port { 0 };
};

// 逐族绑定结果。served 是"本实例实际服务哪些族"的唯一真相源。
struct UdpBindOutcome {
    FamilySet served;
    std::optional<NetError> v4_error; // 请求了、但失败
    std::optional<NetError> v6_error;
};

class UdpServer final {
public:
    UdpServer(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sessions);
    ~UdpServer();

    UdpServer(const UdpServer&) = delete;
    UdpServer& operator=(const UdpServer&) = delete;

    // 单族便捷重载：地址的族决定用哪条 socket（`0.0.0.0` → v4，`::` → v6）。
    // 语义等价于"只存在该族、且 required=true"的计划：失败直接返回错误。
    // 保留给单族调用方与既有测试。
    [[nodiscard]] std::expected<void, NetError> bind(const std::string& bind_ip,
        std::uint16_t port);

    // 逐族绑定。返回每族的实际结果；**不**决定"失败该不该致命"（策略在调用方）。
    [[nodiscard]] UdpBindOutcome bind(const UdpBindPlan& plan);

    // 为已绑定的每一族启动接收循环。任一族启动失败 => 回滚（停掉所有族）并返回错误：
    // "绑定了但收不到"是最难诊断的状态，宁可整体失败也不留半个数据面。
    [[nodiscard]] std::expected<void, NetError> start();
    void stop() noexcept;

    // 广播同一份已编码 datagram（按会话族路由）；非实时线程调用。实现要求同一时刻
    // 只有一个 dispatcher/owner 调用方，因为内部复用 connected_scratch_。
    // 返回值：nullopt = 抛异常；0 = 无接收者；>0 = 实际入队的目标数。
    [[nodiscard]] std::optional<std::size_t> broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept;

    // 两族合计（仅用于既有诊断口径；逐族拆分请用 stats(family)）。
    [[nodiscard]] UdpTransportStats stats() const noexcept;
    [[nodiscard]] UdpTransportStats stats(IpFamily family) const noexcept;

    // 已绑定族中的本地 endpoint（两族同端口，端口恒一致）；未绑定任何族时端口为 0。
    // 需要确定某一族请用 local_endpoint(family)。
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint(IpFamily family) const noexcept;

    // 已**成功绑定**的族。`bind()` 的成败是唯一写入者，`stop()` 不清除它
    // ——因此停机后仍能回答"当时服务了哪些族"（诊断与"绑定失败即致命"的判定都依赖这条语义）。
    // 未 bind 过或 bind 失败的族恒为 false。
    [[nodiscard]] FamilySet served_families() const noexcept;

    [[nodiscard]] std::uint64_t heartbeat_received() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_rejected() const noexcept;
    // 建连/续命计数不由本层重复记录：唯一真相源是 SessionManager 的
    // connected_/refreshed_，此处只做透传（与 session_stats() 同源，
    // GUI 读哪边都一致）。
    [[nodiscard]] std::uint64_t sessions_established() const noexcept;
    [[nodiscard]] std::uint64_t sessions_refreshed() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_ack_attempts() const noexcept;
    [[nodiscard]] std::uint64_t malformed_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t non_heartbeat_datagrams() const noexcept;

private:
    struct State {
        State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess);
        // 两条 transport 恒存在（构造很轻：只建 strand 与未打开的 socket），
        // 是否可用由 bound 标志表达。
        std::shared_ptr<UdpTransport> v4_transport;
        std::shared_ptr<UdpTransport> v6_transport;
        std::shared_ptr<session::SessionManager> sessions;

        std::atomic<bool> v4_bound { false };
        std::atomic<bool> v6_bound { false };

        // 计数为两族合计（既有诊断口径不变）。
        std::atomic<std::uint64_t> heartbeat_received { 0 };
        std::atomic<std::uint64_t> heartbeat_rejected { 0 };
        std::atomic<std::uint64_t> heartbeat_ack_attempts { 0 };
        std::atomic<std::uint64_t> malformed_datagrams { 0 };
        std::atomic<std::uint64_t> non_heartbeat_datagrams { 0 };

        [[nodiscard]] bool is_bound(IpFamily family) const noexcept
        {
            return family == IpFamily::V4 ? v4_bound.load(std::memory_order_acquire)
                                          : (family == IpFamily::V6 && v6_bound.load(std::memory_order_acquire));
        }
        // 未绑定的族返回 nullptr（调用方据此跳过）。
        [[nodiscard]] std::shared_ptr<UdpTransport> transport_for(IpFamily family) const noexcept
        {
            if (!is_bound(family)) {
                return nullptr;
            }
            return family == IpFamily::V4 ? v4_transport : v6_transport;
        }
        [[nodiscard]] FamilySet served() const noexcept
        {
            return FamilySet { v4_bound.load(std::memory_order_acquire),
                v6_bound.load(std::memory_order_acquire) };
        }
    };

    std::shared_ptr<State> state_;
    // broadcast() 有意绑定到 AudioNetworkDispatcher 的单一 worker 线程。
    // 复用这块临时存储，避免每个 AudioFrame 都做一次 vector 分配。
    std::vector<session::SessionManager::ConnectedSession> connected_scratch_;
    // 上次广播的接收端集合（只在 SERVER_RT 宏开时比对更新；无门构建下恒空，
    // 不占堆内存）。成员本身不能 #if（跨 TU 布局一致性），逻辑门控即可；
    // 门关掉时它在 clang -Wall 下是未使用私有字段，故显式 [[maybe_unused]]。
    [[maybe_unused]] std::vector<asio::ip::udp::endpoint> last_broadcast_endpoints_;
};

} // namespace aqua::net

#endif // AQUA_NET_UDP_UDP_SERVER_H
