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

std::optional<SessionManager::session_id_t> SessionManager::create_session(net::IpFamily expected_family)
{
    std::unique_lock lock(mutex_);

    // 探测 kSessionIdCollisionAttempts 个候选：命中空位即 break，全部撞车才失败。
    // 循环体内不再单独检查"最后一次"——重新生成放在下一轮开头，失败判断统一
    // 在循环后按"是否用尽尝试次数"给出，语义与实际探测次数一致（原先最后一次
    // 重新生成的 id 未被检查就被丢弃，白抽一次）。
    constexpr int kSessionIdCollisionAttempts = 100;
    session_id_t id = generate_session_id();
    int attempts = 1;
    while (sessions_.contains(id)) {
        if (attempts >= kSessionIdCollisionAttempts) {
            log_error("create_session: session id collision retries exhausted");
            return std::nullopt;
        }
        id = generate_session_id();
        ++attempts;
    }

    SessionInfo info;
    info.session_id = id;
    // Connect 期的 peer 族：给"这一族有人在等首包"的健康记账用（见头文件）。
    info.expected_family = expected_family;
    info.created_at = std::chrono::steady_clock::now();
    info.last_seen = info.created_at;
    info.state = SessionState::Created;

    sessions_.emplace(id, std::move(info));
    // 新 session 是 Created 而非 Connected，快照内容不变——不重建。
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
        republish_connected_locked();
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
    session_id_t id, const asio::ip::udp::endpoint& endpoint, net::IpFamily family)
{
    // family 必须已经是确定值（由收到包的那条 socket 给出）：Unknown 意味着调用方
    // 没有观测到族，此时写入只会留下一个不确定的状态、也无法做族粘性判断。
    if (family != net::IpFamily::V4 && family != net::IpFamily::V6) {
        log_warn_fmt("Session heartbeat rejected: 0x{:08X} arrived with an undetermined address family", id);
        return HeartbeatOutcome::Rejected;
    }
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
    // 族粘性（INV-3）：已建连的 session 只接受**同一族**的 heartbeat。同 id 却换了
    // 族，只可能是旧 socket 残留包或伪造——客户端换族必须重连（新 session），
    // 否则它的 endpoint 会在两条 socket 之间横跳，而回包只可能从其中一条发出去。
    if (it->second.state == SessionState::Connected && it->second.family != family) {
        const auto pinned = it->second.family;
        lock.unlock();
        log_warn_fmt("Session heartbeat rejected: 0x{:08X} is pinned to {} but this heartbeat arrived on {}; "
                     "changing family requires a reconnect (new session)",
            id, net::ip_family_name(pinned), net::ip_family_name(family));
        return HeartbeatOutcome::Rejected;
    }
    const bool was_connected = it->second.state == SessionState::Connected;
    const auto old_endpoint = it->second.endpoint;
    it->second.endpoint = endpoint;
    it->second.family = family;
    it->second.state = SessionState::Connected;
    // last_seen 只在建连跃迁时刷新：续命 heartbeat 只更新 endpoint（漫游），
    // session 存活由 proto Keepalive 刷新（见 touch_session_liveness）。
    // 两层各管一摊，UDP 续命永远续不了已死的控制面。
    if (!was_connected) {
        it->second.last_seen = std::chrono::steady_clock::now();
        connected_.fetch_add(1, std::memory_order_relaxed);
        // Created -> Connected 是**唯一**会让扇出集合变化的 heartbeat 跃迁，必须重建快照。
        republish_connected_locked();
        lock.unlock();
        log_debug_fmt("Session established: 0x{:08X} endpoint={}", id,
            aqua::net::format_host_port(endpoint.address().to_string(), endpoint.port()));
        return HeartbeatOutcome::Established;
    }
    refreshed_.fetch_add(1, std::memory_order_relaxed);
    const bool endpoint_changed = old_endpoint != endpoint;
    // 漫游（已建连但 endpoint 变了）同样改变扇出集合：数据面按快照里的 endpoint
    // 拨号，不重建就会继续往旧地址发。
    if (endpoint_changed) {
        republish_connected_locked();
    }
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
    bool any_connected_removed = false;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.last_seen > timeout) {
            any_connected_removed = any_connected_removed
                || it->second.state == SessionState::Connected;
            removed.push_back(it->first);
            it = sessions_.erase(it);
            removed_.fetch_add(1, std::memory_order_relaxed);
            expired_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
    // 只有摘掉的是 Connected session 才需要重建；摘掉一堆还在等首包的 Created
    // session 不改变扇出集合。
    if (any_connected_removed) {
        republish_connected_locked();
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

SessionManager::PendingFamilyCounts SessionManager::pending_family_counts() const
{
    PendingFamilyCounts counts;
    std::shared_lock lock(mutex_);
    for (const auto& [id, info] : sessions_) {
        (void)id;
        if (info.state != SessionState::Created) {
            continue; // 已建连的走 connected_family_counts（那一份的族是真的）
        }
        switch (info.expected_family) {
        case net::IpFamily::V4:
            ++counts.v4;
            break;
        case net::IpFamily::V6:
            ++counts.v6;
            break;
        default:
            ++counts.unknown;
            break;
        }
    }
    return counts;
}

SessionManager::FamilyCounts SessionManager::connected_family_counts() const
{
    FamilyCounts counts;
    std::shared_lock lock(mutex_);
    for (const auto& [id, info] : sessions_) {
        (void)id;
        if (info.state != SessionState::Connected) {
            continue;
        }
        if (info.family == net::IpFamily::V4) {
            ++counts.v4;
        } else if (info.family == net::IpFamily::V6) {
            ++counts.v6;
        }
    }
    return counts;
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
    republish_connected_locked();
    removed_by_clear_.fetch_add(count, std::memory_order_relaxed);
    removed_.fetch_add(count, std::memory_order_relaxed);
    lock.unlock();
    if (count > 0) {
        log_debug_fmt("clear: removed {} session(s)", count);
    }
    return count;
}

void SessionManager::republish_connected_locked()
{
    // 调用方必须持有 mutex_（unique 即可）。按当前 sessions_ 重建不可变快照并发布。
    // 分配可能抛 bad_alloc —— 调用点都在 noexcept 成员里，因此必须就地兜住：
    // 发布失败时保留旧快照（内容已陈旧但仍然自洽，绝不发布半个 vector）。
    try {
        auto next = std::make_shared<std::vector<ConnectedSession>>();
        next->reserve(sessions_.size());
        for (const auto& [id, info] : sessions_) {
            if (info.state == SessionState::Connected) {
                next->push_back(ConnectedSession { id, info.endpoint, info.family });
            }
        }
        published_connected_.store(std::move(next), std::memory_order_release);
    } catch (const std::exception& e) {
        log_warn_fmt("SessionManager: failed to republish connected snapshot: {}",
            format_exception_message(e));
    } catch (...) {
        log_warn("SessionManager: failed to republish connected snapshot: unknown exception");
    }
}

std::shared_ptr<const std::vector<SessionManager::ConnectedSession>>
SessionManager::connected_snapshot() const
{
    // 构造期之后首次发布之前理论上读到 nullptr（构造函数不建快照：此时必然为空，
    // 空 vector 与 nullptr 语义相同）。给一个静态空快照兜住，数据面无需判空。
    auto snap = published_connected_.load(std::memory_order_acquire);
    if (!snap) {
        static const auto empty = std::make_shared<const std::vector<ConnectedSession>>();
        return empty;
    }
    return snap;
}

SessionManager::session_id_t SessionManager::generate_session_id()
{
    // session_id 是 HeartbeatAck 阶段唯一的身份凭据，应当不可预测（旧的 16-bit
    // instance + 自增 counter 会让观察者推断出后续 id）。实现是 thread_local
    // mt19937_64（random_device 播种一次：Windows=BCryptGenRandom，
    // Linux/Android=/dev/urandom），多 Server/测试并行无竞争——注意 mt19937 输出
    // 可逆，不要把它当成每会话独立的强随机熵源；威胁模型只假设局域网被动观察者。
    // 0 保留为无效值（ConnectResult::is_valid）；碰撞由 create_session 的重试循环处理。
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
