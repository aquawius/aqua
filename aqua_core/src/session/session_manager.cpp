#include "aqua/session/session_manager.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <random>

namespace aqua::session {

SessionManager::SessionManager()
{
    log_debug("SessionManager created");
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
    created_.fetch_add(1, std::memory_order_relaxed);
    const auto total = sessions_.size();
    lock.unlock();
    log_debug_fmt("Session created internally: id=0x{:08X} state=Created total={}", id, total);
    log_info_fmt("Session created: 0x{:08X} (total={})", id, total);
    return id;
}

bool SessionManager::remove_session(session_id_t id)
{
    std::unique_lock lock(mutex_);
    const bool erased = sessions_.erase(id) > 0;
    if (erased) {
        removed_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto remaining = sessions_.size();
    lock.unlock();
    if (erased) {
        log_debug_fmt("Session removed internally: id=0x{:08X} remaining={}", id, remaining);
        log_info_fmt("Session removed: 0x{:08X} (remaining={})", id, remaining);
    } else {
        log_trace_fmt("Session remove ignored: id=0x{:08X} not found", id);
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
        log_trace_fmt("Session HELLO rejected: invalid endpoint={}",
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
        return false;
    }

    // 信任模型（见 aqua_core/doc/audio_design.md §7 及 UDP 协议注释）：HELLO 只携带
    // session_id，没有任何鉴权。任何知道合法 session_id 的主机都可以伪造 HELLO
    // 覆盖该 session 的 endpoint，把别人的音频流引到自己（或恶意把 endpoint 指
    // 向第三者实施放大）。这在"可信内网"的设计假设下可接受；公网部署前需要
    // 在 ConnectResponse 下发随机 token 并让 HELLO 携带校验。
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        log_trace_fmt("Session HELLO rejected: id=0x{:08X} not found", id);
        return false;
    }
    const bool was_connected = it->second.state == SessionState::Connected;
    it->second.endpoint = endpoint;
    it->second.state = SessionState::Connected;
    it->second.last_seen = std::chrono::steady_clock::now();
    if (was_connected) {
        refreshed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        connected_.fetch_add(1, std::memory_order_relaxed);
    }
    if (was_connected) {
        log_trace_fmt("Session refreshed: 0x{:08X} endpoint={}", id,
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
    } else {
        log_debug_fmt("Session established: 0x{:08X} endpoint={}", id,
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
    }
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
            removed_.fetch_add(1, std::memory_order_relaxed);
            expired_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
    lock.unlock();
    if (!removed.empty()) {
        log_debug_fmt("remove_expired_sessions: removed {} session(s)", removed.size());
    } else {
        log_trace("remove_expired_sessions: no expired sessions");
    }
    return removed;
}

size_t SessionManager::session_count() const
{
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

SessionManager::Stats SessionManager::stats() const noexcept
{
    Stats s;
    s.created = created_.load(std::memory_order_relaxed);
    s.connected = connected_.load(std::memory_order_relaxed);
    s.refreshed = refreshed_.load(std::memory_order_relaxed);
    s.removed = removed_.load(std::memory_order_relaxed);
    s.expired = expired_.load(std::memory_order_relaxed);
    s.clear_removed = clear_removed_.load(std::memory_order_relaxed);
    return s;
}

size_t SessionManager::clear()
{
    std::unique_lock lock(mutex_);
    auto count = sessions_.size();
    sessions_.clear();
    clear_removed_.fetch_add(count, std::memory_order_relaxed);
    removed_.fetch_add(count, std::memory_order_relaxed);
    lock.unlock();
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
    log_trace_fmt("SessionManager snapshot_connected: {} endpoint(s)", out.size());
}

SessionManager::session_id_t SessionManager::generate_session_id()
{
    // 每个 session 用独立的强随机 32 位标识（std::random_device：Windows=BCryptGenRandom，
    // Linux/Android=/dev/urandom）。session_id 是 HELLO_ACK 阶段唯一的身份凭据，
    // 必须不可预测——旧的 16-bit instance + 自增 counter 会让观察者推断出后续 id。
    // 0 保留为无效值（ConnectResult::is_valid）；碰撞由 create_session 的重试循环处理。
    // 调用方（create_session）持有 mutex_，因此这里的 static 随机源是单线程访问。
    static std::random_device rng;
    session_id_t id = 0;
    do {
        id = static_cast<session_id_t>(rng());
    } while (id == 0);
    return id;
}

} // namespace aqua::session
