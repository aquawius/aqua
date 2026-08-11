#ifndef AQUA_SHARE_SESSION_MANAGER_H
#define AQUA_SHARE_SESSION_MANAGER_H

#pragma once

#include <asio.hpp>

#include <chrono>
#include <optional>
#include <random>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace aqua {

class SessionManager {
public:
    using session_id_t = std::uint32_t;

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

    // 创建新的session, 返回session uuid
    std::optional<session_id_t> create_session();

    // 删除session
    bool remove_session(session_id_t id);

    // 查询session
    std::optional<SessionInfo> get_session(session_id_t id) const;

    // UDP发送音频时需要得到endpoint
    std::optional<asio::ip::udp::endpoint> get_endpoint(session_id_t id) const;

    // UDP首次握手: client发送: HELLO + session_id , server记录NAT后的ip:port
    bool establish_udp(session_id_t id, const asio::ip::udp::endpoint& endpoint);

    // UDP收到数据包, 更新last_seen
    bool touch_session(session_id_t id);

    // connected or not.
    bool is_connected(const session_id_t& session_id) const;

    // 查找已经超时的session
    std::vector<session_id_t> collect_expired_sessions(std::chrono::seconds timeout);

    // session 数量查询
    size_t session_count() const;

private:
    session_id_t generate_session_id();

private:
    std::unordered_map<session_id_t, SessionInfo> sessions_;
    mutable std::shared_mutex mutex_;
    uint16_t instance_id_;
    uint16_t counter_;
};

} // namespace aqua

#endif // AQUA_SHARE_SESSION_MANAGER_H
