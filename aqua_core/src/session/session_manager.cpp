#include "aqua/session/session_manager.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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
    for (int attempts = 0; attempts < 100; ++attempts) {
        if (!sessions_.contains(id)) {
            break;
        }
        id = generate_session_id();
        if (attempts == 99) {
            log_error("create_session: session id collision retries exhausted");
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

SessionManager::HeartbeatOutcome SessionManager::on_heartbeat(
    session_id_t id, const asio::ip::udp::endpoint& endpoint)
{
    if (endpoint.port() == 0 || endpoint.address().is_unspecified()) {
        log_trace_fmt("Session heartbeat rejected: invalid endpoint={}",
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
        return HeartbeatOutcome::Rejected;
    }

    // 信任模型（见 aqua_core/doc/audio_design.md §7 及 UDP 协议注释）：heartbeat 只携带
    // session_id，没有任何鉴权。任何知道合法 session_id 的主机都可以伪造 heartbeat
    // 覆盖该 session 的 endpoint，把别人的音频流引到自己（或恶意把 endpoint 指
    // 向第三者实施放大）。这在"可信内网"的设计假设下可接受；公网部署前需要
    // 在 ConnectResponse 下发随机 token 并让 heartbeat 携带校验。
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        log_trace_fmt("Session heartbeat rejected: id=0x{:08X} not found", id);
        return HeartbeatOutcome::Rejected;
    }
    const bool was_connected = it->second.state == SessionState::Connected;
    const auto old_endpoint = it->second.endpoint;
    it->second.endpoint = endpoint;
    it->second.state = SessionState::Connected;
    // last_seen 只在建连跃迁时刷新：续命 heartbeat 只更新 endpoint（漫游），
    // session 存活由 proto Keepalive 刷新（见 touch_session_liveness）。
    // 两层各管一摊，UDP 续命永远续不了已死的控制面。
    if (!was_connected) {
        it->second.last_seen = std::chrono::steady_clock::now();
        connected_.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        log_debug_fmt("Session established: 0x{:08X} endpoint={}", id,
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
        return HeartbeatOutcome::Established;
    }
    refreshed_.fetch_add(1, std::memory_order_relaxed);
    const bool endpoint_changed = old_endpoint != endpoint;
    lock.unlock();
    if (endpoint_changed) {
        // client 在 NAT 之后，自己感知不到映射变化（重绑/漫游/IPv6 轮换）；
        // server 侧是唯一能看见新地址的一方，这里是唯一的跟随点，值得 info。
        log_info_fmt("Session endpoint changed: 0x{:08X} {} -> {}", id,
            aqua::net::format_host_port(old_endpoint.address().to_string(), old_endpoint.port()),
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
    } else {
        log_trace_fmt("Session refreshed: 0x{:08X} endpoint={}", id,
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
    }
    return HeartbeatOutcome::Refreshed;
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

bool SessionManager::touch_session_liveness(session_id_t id)
{
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second.last_seen = std::chrono::steady_clock::now();
    return true;
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

SessionManager::ActivityAge SessionManager::activity_age() const
{
    ActivityAge age;
    std::shared_lock lock(mutex_);
    if (sessions_.empty()) {
        return age;
    }
    const auto now = std::chrono::steady_clock::now();
    // 显式循环而非 `views::values | views::transform + ranges::minmax`：
    // NDK r30 的 libc++ 会拒绝 elements_view 的管道组合（MSVC 接受），
    // 跨标准库可移植性不值得用这点语法糖换。另外 elapsed（milliseconds::rep
    // = long long）与 int64_t 在 LP64（Android/Linux）是 long、在 LLP64
    // （Windows）是 long long——混用会让 std::max 推导失败，故统一在
    // uint64 域比较。
    bool have_sample = false;
    for (const auto& [id, info] : sessions_) {
        (void)id;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - info.last_seen)
                                 .count();
        // 负值钳 0：时钟回拨 / 并发刷新防御。
        const auto ms = static_cast<std::uint64_t>(elapsed > 0 ? elapsed : 0);
        if (!have_sample) {
            age.oldest_age_ms = ms;
            age.newest_age_ms = ms;
            have_sample = true;
        } else {
            age.oldest_age_ms = std::max(age.oldest_age_ms, ms);
            age.newest_age_ms = std::min(age.newest_age_ms, ms);
        }
    }
    age.count = sessions_.size();
    return age;
}

SessionManager::Stats SessionManager::stats() const noexcept
{
    Stats s;
    s.created = created_.load(std::memory_order_relaxed);
    s.connected = connected_.load(std::memory_order_relaxed);
    s.refreshed = refreshed_.load(std::memory_order_relaxed);
    s.removed = removed_.load(std::memory_order_relaxed);
    s.expired = expired_.load(std::memory_order_relaxed);
    s.removed_by_clear = removed_by_clear_.load(std::memory_order_relaxed);
    return s;
}

size_t SessionManager::clear()
{
    std::unique_lock lock(mutex_);
    auto count = sessions_.size();
    sessions_.clear();
    removed_by_clear_.fetch_add(count, std::memory_order_relaxed);
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
    // Linux/Android=/dev/urandom）。session_id 是 HeartbeatAck 阶段唯一的身份凭据，
    // 必须不可预测——旧的 16-bit instance + 自增 counter 会让观察者推断出后续 id。
    // 0 保留为无效值（ConnectResult::is_valid）；碰撞由 create_session 的重试循环处理。
    // 调用方（create_session）持有 mutex_，但 static 随机源是跨实例全局共享：
    // 用 thread_local mt19937_64（random_device 播种一次），多 Server/测试并行也无竞争。
    thread_local std::mt19937_64 rng { [] {
        std::random_device rd;
        return static_cast<std::uint64_t>(rd()) << 32 | rd();
    }() };
    session_id_t id = 0;
    do {
        id = static_cast<session_id_t>(rng());
    } while (id == 0);
    return id;
}

} // namespace aqua::session
