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

bool session_manager::add_session(std::string client_uuid, const asio::ip::udp::endpoint& endpoint)
{
    std::unique_lock lock(m_mutex);

    // 检查是否存在相同 endpoint
    for (const auto& [existing_uuid, existing_session] : m_sessions_map) {
        if (existing_session->get_endpoint() == endpoint) {
            // 已存在相同的 IP+端口
            spdlog::warn("[session_manager] Duplicate endpoint found ({}:{}) for UUID={}.",
                endpoint.address().to_string(),
                endpoint.port(),
                existing_uuid);
            // 根据需求，决定是返回 false、还是覆盖旧会话，或做其他处理
            return false;
        }
    }

    // 如果没有重复 endpoint，就检查同 UUID 是否存在（如下示例依旧沿用现有逻辑）
    auto it = m_sessions_map.find(client_uuid);
    if (it != m_sessions_map.end()) {
        // 已存在同名 session，先断开旧的再创建新的
        it->second->disconnect();
        it->second = std::make_shared<session>(std::move(client_uuid), endpoint);
        spdlog::info("[session_manager] Session replaced for UUID={}", it->first);
        return false; // 表示是覆盖旧的
    }

    // 如果既不存在相同 endpoint，也没有同 UUID，会添加一个新 session
    m_sessions_map.emplace(client_uuid, std::make_shared<session>(client_uuid, endpoint));
    spdlog::info("[session_manager] New session created: UUID={}, client_ip={} client_port={}", client_uuid, endpoint.address().to_string(), endpoint.port());
    return true;
}

void session_manager::remove_session(const std::string& client_uuid)
{
    std::unique_lock lock(m_mutex);
    auto erased = m_sessions_map.erase(client_uuid);
    if (erased > 0) {
        spdlog::info("[session_manager] Session removed for UUID={}", client_uuid);
    } else {
        spdlog::warn("[session_manager] Remove session called, but UUID={} not found.", client_uuid);
    }
}

std::vector<asio::ip::udp::endpoint> session_manager::get_active_endpoints()
{
    std::unique_lock lock(m_mutex);
    std::vector<asio::ip::udp::endpoint> endpoints;
    endpoints.reserve(m_sessions_map.size());

    for (const auto& [uuid, session_ptr] : m_sessions_map) {
        if (session_ptr->is_connected() && session_ptr->is_alive()) {
            endpoints.push_back(session_ptr->get_endpoint());
        } else {
            spdlog::debug("[session_manager] Skipping inactive session: UUID={}", uuid);
        }
    }

    return endpoints;
}

size_t session_manager::get_session_count() const
{
    std::unique_lock lock(m_mutex);
    auto count = m_sessions_map.size();
    spdlog::trace("[session_manager] get_session_count: {}", count);
    return count;
}

std::optional<std::shared_ptr<session>> session_manager::get_session(const std::string& client_uuid)
{
    std::unique_lock lock(m_mutex);
    auto it = m_sessions_map.find(client_uuid);
    if (it != m_sessions_map.end()) {
        spdlog::trace("[session_manager] get_session found UUID={}", client_uuid);
        return it->second;
    }
    spdlog::warn("[session_manager] get_session could not find UUID={}", client_uuid);
    return std::nullopt;
}

std::vector<std::shared_ptr<session>> session_manager::get_sessions() const
{
    std::vector<std::shared_ptr<session>> sessions;
    std::shared_lock lock(m_mutex);
    for (const auto& pair : m_sessions_map) {
        sessions.push_back(pair.second);
    }
    return sessions;
}

void session_manager::clear_all_sessions()
{
    std::unique_lock lock(m_mutex);
    m_sessions_map.clear();
}

bool session_manager::update_keepalive(const std::string& client_uuid)
{
    std::unique_lock lock(m_mutex);
    auto it = m_sessions_map.find(client_uuid);
    if (it != m_sessions_map.end()) {
        // 检查会话是否已过期
        if (!it->second->is_alive()) {
            spdlog::warn("[session_manager] Session expired for UUID={}", client_uuid);
            return false; // 返回false表示会话已过期
        }
        it->second->update_keepalive();
        spdlog::trace("[session_manager] KeepAlive updated for UUID={}", client_uuid);
        return true;
    } else {
        spdlog::warn("[session_manager] update_keepalive: UUID={} not found.", client_uuid);
        return false;
    }
}

void session_manager::check_sessions()
{
    std::unique_lock lock(m_mutex);
    for (auto it = m_sessions_map.begin(); it != m_sessions_map.end();) {
        if (!it->second->is_connected() || !it->second->is_alive()) {
            spdlog::info("[session_manager] Session expired or disconnected, removing UUID={}", it->first);
            it = m_sessions_map.erase(it);
        } else {
            ++it;
        }
    }
}