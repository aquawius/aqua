#include "aqua/session/session_manager.h"

#include "aqua/logger/logger.h"

#include <random>

namespace aqua::session {

SessionManager::SessionManager()
    // instance_id 用 | 1 强制最低位为 1，保证 >= 1。这样 session_id 的高 16 位恒非零，
    // session_id 永远不可能是 0（0 保留给 UDP 音频广播标记，见 net::kBroadcastSessionId）。
    // 熵从 16 bit 降到 15 bit（32768 种 instance_id），跨进程区分已足够。
    : instance_id_(static_cast<std::uint16_t>(std::random_device { }() | 1u))
    , counter_(static_cast<std::uint16_t>(std::random_device { }()))
{
    log_debug_fmt("SessionManager created (instance_id=0x{:04X})", instance_id_);
}

SessionManager::~SessionManager()
{
    std::shared_lock lock(mutex_);
    auto count = sessions_.size();
    lock.unlock();
    if (count > 0) {
        log_warn_fmt("SessionManager destroyed with {} session(s) still alive", count);
    }
}

std::optional<SessionManager::session_id_t> SessionManager::create_session()
{
    std::unique_lock lock(mutex_);

    session_id_t id = generate_session_id();
    const session_id_t start = id;
    while (sessions_.contains(id)) {
        id = generate_session_id();
        if (id == start) {
            log_error("create_session: session id space exhausted");
            return std::nullopt;
        }
    }

    SessionInfo info;
    info.session_id = id;
    info.created_at = std::chrono::steady_clock::now();
    info.last_seen = info.created_at;
    info.state = SessionState::Created;

    sessions_.emplace(id, std::move(info));
    log_debug_fmt("create_session: 0x{:08X} (total={})", id, sessions_.size());
    return id;
}

bool SessionManager::remove_session(session_id_t id)
{
    std::unique_lock lock(mutex_);
    bool erased = sessions_.erase(id) > 0;
    if (erased) {
        log_debug_fmt("remove_session: 0x{:08X} (remaining={})", id, sessions_.size());
    }
    return erased;
}

std::optional<SessionManager::SessionInfo> SessionManager::get_session(session_id_t id) const
{
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<asio::ip::udp::endpoint> SessionManager::get_endpoint(session_id_t id) const
{
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    // 未完成 UDP 握手（仍为 Created）时没有有效 endpoint，返回 nullopt，
    // 与 AGENT.md §22.2 契约一致（"不存在或未握手返回 std::nullopt"）。
    if (it->second.state != SessionState::Connected) {
        return std::nullopt;
    }
    return it->second.endpoint;
}

bool SessionManager::establish_session(session_id_t id, const asio::ip::udp::endpoint& endpoint)
{
    if (endpoint.port() == 0 || endpoint.address().is_unspecified()) {
        return false;
    }

    // 信任模型（见 doc/protocol.md「威胁模型与已知限制」）：HELLO 只携带
    // session_id，没有任何鉴权。任何知道合法 session_id 的主机都可以伪造 HELLO
    // 覆盖该 session 的 endpoint，把别人的音频流引到自己（或恶意把 endpoint 指
    // 向第三者实施放大）。这在"可信内网"的设计假设下可接受；公网部署前需要
    // 在 ConnectResponse 下发随机 token 并让 HELLO 携带校验。
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.endpoint = endpoint;
    it->second.state = SessionState::Connected;
    it->second.last_seen = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::touch_session(session_id_t id)
{
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end() || it->second.state != SessionState::Connected) {
        return false;
    }
    it->second.last_seen = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::is_connected(session_id_t session_id) const
{
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    return it->second.state == SessionState::Connected;
}

std::vector<SessionManager::session_id_t> SessionManager::collect_expired_sessions(
    std::chrono::milliseconds timeout) const
{
    std::shared_lock lock(mutex_);
    std::vector<session_id_t> expired;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [id, info] : sessions_) {
        if (now - info.last_seen > timeout) {
            expired.push_back(id);
        }
    }
    return expired;
}

std::vector<SessionManager::session_id_t> SessionManager::remove_expired_sessions(
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    std::vector<session_id_t> removed;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.last_seen > timeout) {
            removed.push_back(it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    if (!removed.empty()) {
        log_debug_fmt("remove_expired_sessions: removed {} session(s)", removed.size());
    }
    return removed;
}

size_t SessionManager::session_count() const
{
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

size_t SessionManager::clear()
{
    std::unique_lock lock(mutex_);
    auto count = sessions_.size();
    sessions_.clear();
    if (count > 0) {
        log_debug_fmt("clear: removed {} session(s)", count);
    }
    return count;
}

void SessionManager::snapshot_connected(std::vector<ConnectedSession>& out) const
{
    out.clear();
    {
        std::shared_lock lock(mutex_);
        // 只在 session 数量增长时扩容；通常 packetizer 会复用同一容量。
        if (out.capacity() < sessions_.size()) {
            out.reserve(sessions_.size());
        }
        for (const auto& [id, info] : sessions_) {
            if (info.state == SessionState::Connected) {
                out.push_back(ConnectedSession { id, info.endpoint });
            }
        }
    }
}

SessionManager::session_id_t SessionManager::generate_session_id()
{
    return (static_cast<std::uint32_t>(instance_id_) << 16) | (++counter_);
}

} // namespace aqua::session
