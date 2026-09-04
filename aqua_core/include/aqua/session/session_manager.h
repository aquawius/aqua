#ifndef AQUA_SESSION_MANAGER_H
#define AQUA_SESSION_MANAGER_H

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
    };

    // SessionManager 只描述“当前仍存在的 session”。移除/超时后对象直接从表中消失，
    // 不额外维护 Expired/Closed 历史状态。
    enum class SessionState : std::uint8_t {
        Created = 0,
        Connected,
    };

    struct SessionInfo {
        session_id_t session_id = 0;
        // 最近一次成功 UDP HELLO 刷新的 NAT 映射地址。Audio datagram 不更新 last_seen。
        asio::ip::udp::endpoint endpoint;
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
        std::uint64_t clear_removed = 0;
    };

    // 当前存活 session 的"最后活动"年龄（诊断 Gauge）。
    // session 的 last_seen 只由 UDP HELLO 刷新（Audio datagram 不刷新），
    // 因此 age 直接回答"这个 client 多久没保活了"——active=1 但 age=4900ms
    // 意味着它下一轮就会被 reap（session_timeout 默认 5s）。
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
    std::optional<session_id_t> create_session();

    // 删除 session；不存在返回 false。
    bool remove_session(session_id_t id);

    // 查询 session 快照。
    std::optional<SessionInfo> get_session(session_id_t id) const;

    // 获取已完成 UDP 握手的 NAT endpoint；Created 状态返回 nullopt。
    std::optional<asio::ip::udp::endpoint> get_endpoint(session_id_t id) const;

    // UDP HELLO：记录/刷新 NAT endpoint，并将 session 置为 Connected。
    // endpoint.port()==0 或 endpoint.address().is_unspecified() 的输入视为非法。
    bool establish_session(session_id_t id, const asio::ip::udp::endpoint& endpoint);

    [[nodiscard]] bool is_connected(session_id_t session_id) const;

    // 在同一把锁内判断并删除超时 session，避免扫描后再次判断产生 TOCTOU。
    std::vector<session_id_t> remove_expired_sessions(std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t session_count() const;
    [[nodiscard]] Stats stats() const noexcept;

    // 采集当前存活 session 的最后活动年龄（一次加锁扫描，O(active)）。
    // 诊断冷路径（1s 一次）；session 数为个位数量级。任意线程可调。
    [[nodiscard]] ActivityAge activity_age() const;

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
    std::atomic<std::uint64_t> clear_removed_ { 0 };
};

} // namespace aqua::session

#endif // AQUA_SESSION_MANAGER_H
