#include "core/session/session_manager.h"

#include "core/logger/logger.h"

#include <random>

namespace aqua {

SessionManager::SessionManager()
    : instance_id_(static_cast<uint16_t>(std::random_device { }()))
    , counter_(static_cast<uint16_t>(std::random_device { }()))
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

void SessionManager::for_each_connected(
    const std::function<bool(session_id_t, const asio::ip::udp::endpoint&)>& callback) const
{
    std::shared_lock lock(mutex_);
    // 注意：此函数被 server packetizer 每秒高频调用（约 100 次/秒），
    // 内部不记录日志以避免刷屏。流量统计由调用方在主循环中周期性输出。
    for (const auto& [id, info] : sessions_) {
        if (info.state == SessionState::Connected) {
            if (!callback(id, info.endpoint)) break;
        }
    }
}

SessionManager::session_id_t SessionManager::generate_session_id()
{
    return (static_cast<uint32_t>(instance_id_) << 16) | (++counter_);
}

} // namespace aqua
