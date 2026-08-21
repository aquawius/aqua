#ifndef AQUA_SESSION_MANAGER_H
#define AQUA_SESSION_MANAGER_H

#include <asio.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aqua {

class SessionManager {
public:
    using session_id_t = std::uint32_t;
    using ConnectedSession = std::pair<session_id_t, asio::ip::udp::endpoint>;

    enum class SessionState : uint8_t {
        Created = 0,
        Connecting,
        Connected,
        Expired,
        Closed
    };

    struct SessionInfo {
        // session id
        session_id_t session_id;
        // UDP NAT 映射地址
        asio::ip::udp::endpoint endpoint;
        // 创建时间
        std::chrono::steady_clock::time_point created_at;
        // 最近一次UDP通信
        std::chrono::steady_clock::time_point last_seen;
        // 是否完成UDP握手
        SessionState state = SessionState::Created;
    };

public:
    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // 创建新的session, 返回 session_id
    std::optional<session_id_t> create_session();

    // 删除session
    bool remove_session(session_id_t id);

    // 查询session
    std::optional<SessionInfo> get_session(session_id_t id) const;

    // UDP发送音频时需要得到endpoint
    std::optional<asio::ip::udp::endpoint> get_endpoint(session_id_t id) const;

    // UDP首次握手: client发送: HELLO + session_id , server记录NAT后的ip:port
    bool establish_session(session_id_t id, const asio::ip::udp::endpoint& endpoint);

    // UDP收到数据包, 更新last_seen
    bool touch_session(session_id_t id);

    // connected or not.
    bool is_connected(const session_id_t& session_id) const;

    // 查找已经超时的session
    std::vector<session_id_t> collect_expired_sessions(std::chrono::seconds timeout);

    // 原子地收集并删除已超时的 session（同一把锁内完成），避免"扫描快照与删除之间
    // 收到 keepalive HELLO 仍被误删"的 TOCTOU。返回被删除的 session_id 列表（供日志）。
    std::vector<session_id_t> remove_expired_sessions(std::chrono::seconds timeout);

    // session 数量查询
    size_t session_count() const;

    // 先清空out的内容, 把当前 Connected session 的 endpoint 快照写入 out。
    // out 的容量由调用方复用，避免 packetizer 每个音频包都重新分配。
    void snapshot_connected(std::vector<ConnectedSession>& out) const;

    // 清空所有 session（用于 server 优雅退出）。
    // 返回被清理的 session 数量。
    size_t clear();

private:
    session_id_t generate_session_id();

private:
    std::unordered_map<session_id_t, SessionInfo> sessions_;
    mutable std::shared_mutex mutex_;
    uint16_t instance_id_; // 恒 >= 1（构造时 |1），保证 session_id 高 16 位非零（0 保留给广播）
    uint16_t counter_;
};

} // namespace aqua

#endif // AQUA_SESSION_MANAGER_H
