//
// Created by aquawius on 25-1-19.
//

#include "session_manager.h"
#include <spdlog/spdlog.h>

session_manager& session_manager::get_instance()
{
    static session_manager instance;
    return instance;
}

session_manager::session_manager()
{
    spdlog::debug("[session_manager] Session manager initialized");
}

bool session_manager::add_session(const std::string& uuid, const boost::asio::ip::udp::endpoint& endpoint)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // 检查是否存在相同endpoint
    for (const auto& [existing_uuid, session] : m_sessions) {
        if (session->get_endpoint() == endpoint) {
            spdlog::warn("[session_manager] Duplicate endpoint found ({}:{}) for UUID={}.",
                endpoint.address().to_string(),
                endpoint.port(),
                existing_uuid);
            return false;
        }
    }

    // 如果存在相同UUID，先删除旧会话
    auto it = m_sessions.find(uuid);
    if (it != m_sessions.end()) {
        m_sessions.erase(it);
        spdlog::info("[session_manager] Replaced existing session for UUID={}", uuid);
    }

    // 添加新会话
    auto new_session = std::make_shared<session>(uuid, endpoint);
    m_sessions[uuid] = new_session;

    spdlog::info("[session_manager] New session created: UUID={}, endpoint={}:{}",
        uuid, endpoint.address().to_string(), endpoint.port());
    return true;
}

bool session_manager::remove_session(const std::string& uuid)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto erased = m_sessions.erase(uuid);
    if (erased > 0) {
        spdlog::info("[session_manager] Session removed for UUID={}", uuid);
        return true;
    } else {
        spdlog::warn("[session_manager] Remove session called, but UUID={} not found.", uuid);
        return false;
    }
}

bool session_manager::update_keepalive(const std::string& uuid)
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_sessions.find(uuid);
    if (it != m_sessions.end()) {
        auto& session = it->second;
        if (!session->is_alive()) {
            spdlog::warn("[session_manager] Session expired for UUID={}", uuid);
            return false;
        }
        session->update_keepalive();
        spdlog::trace("[session_manager] KeepAlive updated for UUID={}", uuid);
        return true;
    } else {
        spdlog::warn("[session_manager] update_keepalive: UUID={} not found.", uuid);
        return false;
    }
}

bool session_manager::is_session_valid(const std::string& uuid)
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_sessions.find(uuid);
    if (it == m_sessions.end()) {
        spdlog::warn("[session_manager] Session validation failed: UUID={} not found", uuid);
        return false;
    }

    if (!it->second->is_alive()) {
        spdlog::warn("[session_manager] Session validation failed: UUID={} expired", uuid);
        return false;
    }

    spdlog::trace("[session_manager] Session validation passed: UUID={}", uuid);
    return true;
}

void session_manager::check_sessions()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (!it->second->is_alive()) {
            spdlog::info("[session_manager] Session expired, removing UUID={}", it->first);
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }

    spdlog::trace("[session_manager] Session check completed, active sessions: {}", m_sessions.size());
}

void session_manager::clear_all_sessions()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_sessions.clear();
    spdlog::info("[session_manager] All sessions cleared");
}

size_t session_manager::get_session_count() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_sessions.size();
}

std::vector<boost::asio::ip::udp::endpoint> session_manager::get_active_endpoints() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<boost::asio::ip::udp::endpoint> endpoints;
    endpoints.reserve(m_sessions.size());

    for (const auto& [uuid, session] : m_sessions) {
        if (session->is_alive()) {
            endpoints.push_back(session->get_endpoint());
        }
    }

    return endpoints;
}

std::optional<std::shared_ptr<session>> session_manager::get_session(const std::string& uuid) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_sessions.find(uuid);
    if (it != m_sessions.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::shared_ptr<session>> session_manager::get_sessions() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::shared_ptr<session>> sessions;
    sessions.reserve(m_sessions.size());

    for (const auto& [uuid, session] : m_sessions) {
        if (session->is_alive()) {
            sessions.push_back(session);
        }
    }

    return sessions;
}