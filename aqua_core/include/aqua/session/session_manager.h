#ifndef AQUA_SESSION_MANAGER_H
#define AQUA_SESSION_MANAGER_H

#include <asio.hpp>

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
        // 最近一次成功的 UDP HELLO/数据通信所对应的 NAT 映射地址。
        asio::ip::udp::endpoint endpoint;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_seen;
        SessionState state = SessionState::Created;
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

    // 更新已连接 session 的最近活动时间。Created 状态不会被刷新；通常由已验证的
    // HELLO/音频数据包调用，本函数不承担 packet 鉴权。
    bool touch_session(session_id_t id);

    [[nodiscard]] bool is_connected(session_id_t session_id) const;

    // 返回当前已经超时的 session，但不删除。主要用于诊断/观测。

    // 在同一把锁内判断并删除超时 session，避免扫描后再次判断产生 TOCTOU。
    std::vector<session_id_t> remove_expired_sessions(std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t session_count() const;

    // 将当前 Connected session 快照写入 out；out 会先 clear()，调用方可以复用容量。
    void snapshot_connected(std::vector<ConnectedSession>& out) const;

    // 清空所有 session，用于 server 优雅退出。
    // 返回被清理的数量。
    std::size_t clear();

private:
    session_id_t generate_session_id();

    std::unordered_map<session_id_t, SessionInfo> sessions_;
    mutable std::shared_mutex mutex_;
    std::uint16_t instance_id_ = 0; // 构造器初始化（random_device | 1）
    std::uint16_t counter_ = 0;     // 构造器初始化（random_device）
};

} // namespace aqua::session

#endif // AQUA_SESSION_MANAGER_H
