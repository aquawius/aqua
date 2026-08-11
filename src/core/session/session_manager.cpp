#include "core/session/session_manager.h"

#include <random>

namespace aqua {

SessionManager::SessionManager()
    : instance_id_(static_cast<uint16_t>(std::random_device { }()))
    , counter_(static_cast<uint16_t>(std::random_device { }()))
{
}

SessionManager::~SessionManager() = default;

std::optional<SessionManager::session_id_t> SessionManager::create_session()
{
    std::unique_lock lock(mutex_);

    session_id_t id = generate_session_id();
    const session_id_t start = id;
    while (sessions_.contains(id)) {
        id = generate_session_id();
        if (id == start) {
            return std::nullopt;
        }
    }

    SessionInfo info;
    info.session_id = id;
    info.created_at = std::chrono::steady_clock::now();
    info.last_seen = info.created_at;
    info.state = SessionState::Created;

    sessions_.emplace(id, std::move(info));
    return id;
}

bool SessionManager::remove_session(session_id_t id)
{
    std::unique_lock lock(mutex_);
    return sessions_.erase(id) > 0;
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
    return it->second.endpoint;
}

bool SessionManager::establish_udp(session_id_t id, const asio::ip::udp::endpoint& endpoint)
{
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
    if (it == sessions_.end()) {
        return false;
    }
    it->second.last_seen = std::chrono::steady_clock::now();
    return true;
}

bool SessionManager::is_connected(const session_id_t& session_id) const
{
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }
    return it->second.state == SessionState::Connected;
}

std::vector<SessionManager::session_id_t> SessionManager::collect_expired_sessions(
    std::chrono::seconds timeout)
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

size_t SessionManager::session_count() const
{
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

SessionManager::session_id_t SessionManager::generate_session_id()
{
    return (static_cast<uint32_t>(instance_id_) << 16) | (++counter_);
}

} // namespace aqua
