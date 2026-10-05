#ifndef AQUA_SESSION_SESSION_MANAGER_H
#define AQUA_SESSION_SESSION_MANAGER_H

#include "aqua/net/address/ip_family.h"

#include <asio.hpp>

#include <atomic>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace aqua::session {

class SessionManager {
public:
    using session_id_t = std::uint32_t;

    struct ConnectedSession {
        session_id_t session_id = 0;
        asio::ip::udp::endpoint endpoint;
        // 该会话被观测到的地址族（= 收到并写入 endpoint 的那条 socket 的族）。
        // 与 endpoint 在**同一把锁内一起写入**，因此两者永不发散；扇出按它路由
        // 到对应族的 socket（跨族发送会 EAFNOSUPPORT）。
        net::IpFamily family { net::IpFamily::Unknown };
    };

    // SessionManager 只描述“当前仍存在的 session”。移除/超时后对象直接从表中消失，
    // 不额外维护 Expired/Closed 历史状态。
    //
    // 所有权分层（存活分两层，各管一摊）：
    //   Created   = gRPC Connect 已分配 session_id，等 UDP 首包。无 endpoint，
    //               server 还不知道 client 从哪个公网地址打过来。
    //   Connected = UDP heartbeat 已建连，endpoint 已知（NAT 映射地址）。
    // last_seen 归 gRPC 层（proto Keepalive 经 touch_session_liveness 刷新）；
    // endpoint + Connected 状态归 UDP 层（on_heartbeat 维护）。
    enum class SessionState : std::uint8_t {
        Created = 0,
        Connected,
    };

    // UDP heartbeat 的处理结果（三态：调用方靠它区分建连/续命记账与日志；
    // 非 Rejected 的包 server 都回 HeartbeatAck）。
    //   Rejected    = 非法 endpoint 或未知 session，本包无任何副作用。
    //   Established = 首包：Created→Connected，记 endpoint 并刷新 last_seen
    //                 （桥接 Connect 到首次 Keepalive 之间的空窗）。
    //   Refreshed   = 续命包：只覆盖 endpoint（NAT 重绑/漫游/IPv6 轮换时静默跟随），
    //                 不碰 last_seen(有grpc刷新)。
    enum class HeartbeatOutcome : std::uint8_t {
        Rejected = 0,
        Established,
        Refreshed,
    };

    struct SessionInfo {
        session_id_t session_id = 0;
        // 最近一次 heartbeat 刷新的 NAT 映射地址。Audio datagram 不更新 last_seen。
        asio::ip::udp::endpoint endpoint;
        // 首包（Created→Connected）钉住的地址族。Unknown = 还没收到过 heartbeat
        // （族只能由"收到包的那条 socket"观测到，Connect 阶段无法确定）。
        net::IpFamily family { net::IpFamily::Unknown };
        // Connect 期从控制面 peer 观测到的期望族（Unknown = 解析不出来）。仅用于
        // "这一族有人在等首包"的健康记账，不参与路由与扇出（那些只看上面的 family）。
        net::IpFamily expected_family { net::IpFamily::Unknown };
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_seen;
        SessionState state = SessionState::Created;
    };

    struct Stats {
        std::uint64_t created = 0;
        std::uint64_t connected = 0;
        std::uint64_t refreshed = 0;
        std::uint64_t removed = 0;
        std::uint64_t expired = 0;
        std::uint64_t removed_by_clear = 0;
    };

    // 当前存活 session 的"最后活动"年龄（诊断 Gauge）。
    // session 的 last_seen 由 proto Keepalive 刷新（UDP heartbeat 只在建连跃迁时
    // 刷新一次、续命不再碰），因此 age 直接回答"这个 client 的控制面多久没
    // 探活了"——active=1 但 age=29s 意味着它下一轮就会被 reap
    // （session_timeout 默认 5s）。
    // 多 session 场景下 oldest/newest 比单一 age 更有意义（最老的那个
    // 才是即将超时/已经半死的连接）。无存活 session 时两者均为 0。
    struct ActivityAge {
        std::size_t count = 0;
        std::uint64_t oldest_age_ms = 0;
        std::uint64_t newest_age_ms = 0;
    };

    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // 创建新的 session。ID 0 保留，不会返回。
    //
    // expected_family = **控制面观测到的**客户端族（Connect 的 peer 族；解析不出来传
    // Unknown）。它只用于"这一族有人在等首包"的健康记账：真正生效的族仍由首个
    // heartbeat 落到哪条 socket 钉住（INV-3，见 SessionInfo::family），两者可以不同。
    std::optional<session_id_t> create_session(net::IpFamily expected_family = net::IpFamily::Unknown);

    // 删除 session；不存在返回 false。
    bool remove_session(session_id_t id);

    // 查询 session 快照。
    std::optional<SessionInfo> get_session(session_id_t id) const;

    // 获取已完成 UDP 握手的 NAT endpoint；Created 状态返回 nullopt。
    std::optional<asio::ip::udp::endpoint> get_endpoint(session_id_t id) const;

    // UDP heartbeat 入口：首包建立 association（并**钉住地址族**），之后只刷新
    // NAT endpoint（漫游续命，不碰 last_seen）。session 存活由 proto Keepalive 刷新。
    //
    // family 由**收到该包的那条 socket**给出（不是从地址反推）。拒绝条件：
    //   - family 不是 V4/V6（调用方没观测到族，写进去只会得到一个不确定的状态）；
    //   - endpoint.port()==0 或 address().is_unspecified()；
    //   - session 不存在；
    //   - **同 id 但来自另一族**（族粘性 INV-3）：一个 session 的两条 socket 只可能
    //     是旧连接残留或伪造 —— 换族必须重连（= 新 session）。
    HeartbeatOutcome on_heartbeat(session_id_t id, const asio::ip::udp::endpoint& endpoint,
        net::IpFamily family);

    // proto Keepalive 的存活刷新：session 存在即更新 last_seen 并返回 true，
    // 不存在返回 false（调用方应停止，而非重试）。不碰 endpoint 与状态。
    bool touch_session_liveness(session_id_t id);

    [[nodiscard]] bool is_connected(session_id_t session_id) const;

    // 在同一把锁内判断并删除超时 session，避免扫描后再次判断产生 TOCTOU。
    std::vector<session_id_t> remove_expired_sessions(std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t session_count() const;
    [[nodiscard]] Stats stats() const noexcept;

    // 采集当前存活 session 的最后活动年龄（一次加锁扫描，O(active)）。
    // 诊断冷路径（1s 一次）；session 数为个位数量级。任意线程可调。
    [[nodiscard]] ActivityAge activity_age() const;

    // 已建连（Connected）session 按族分布。诊断冷路径用（1s 一次，一次加锁扫描）。
    // 未建连的 session（族未知）不计入任何一族。
    struct FamilyCounts {
        std::size_t v4 = 0;
        std::size_t v6 = 0;
    };
    [[nodiscard]] FamilyCounts connected_family_counts() const;

    // 还没建连（state == Created）的 session 按**期望族**分布。
    // 用途：收包健康检查要回答"这一族有没有人在等首包"——此时真正的族还没被观测到
    // （未建连的 session 在 connected_family_counts 里不属于任何一族），只有 Connect
    // 期的 peer 族能回答，所以必须在建会话时记一份（见 create_session）。
    // unknown 桶 = peer 族解析失败的 session（fail-open 路径）；调用方不得据此断定
    // "没人等"——它只是"不知道"。
    struct PendingFamilyCounts {
        std::size_t v4 = 0;
        std::size_t v6 = 0;
        std::size_t unknown = 0;
    };
    [[nodiscard]] PendingFamilyCounts pending_family_counts() const;

    // 将当前 Connected session 快照写入 out；out 会先 clear()，调用方可以复用容量。
    void snapshot_connected(std::vector<ConnectedSession>& out) const;

    // 清空所有 session，用于 server 优雅退出。
    // 返回被清理的数量。
    std::size_t clear();

private:
    session_id_t generate_session_id();

    std::unordered_map<session_id_t, SessionInfo> sessions_;
    mutable std::shared_mutex mutex_;
    std::atomic<std::uint64_t> created_ { 0 };
    std::atomic<std::uint64_t> connected_ { 0 };
    std::atomic<std::uint64_t> refreshed_ { 0 };
    std::atomic<std::uint64_t> removed_ { 0 };
    std::atomic<std::uint64_t> expired_ { 0 };
    std::atomic<std::uint64_t> removed_by_clear_ { 0 };
};

} // namespace aqua::session

#endif // AQUA_SESSION_SESSION_MANAGER_H
