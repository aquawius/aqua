//
// Created by aquawius on 25-1-19.
//

#include "session_manager.h"

session_manager& session_manager::get_instance()
{
    static session_manager instance;
    return instance;
}
bool session_manager::add_session(std::string client_id, const asio::ip::udp::endpoint& endpoint)
{
    std::unique_lock lock(m_mutex);
    auto it = m_sessions_map.find(client_id);
    if (it != m_sessions_map.end()) { // 已存在同名 session，示例逻辑为先断开旧的，然后覆盖
        it->second->disconnect();
        it->second = std::make_shared<session>(std::move(client_id), endpoint);
        return false; // 表示是覆盖旧的，而不是新建
    }
    m_sessions_map.emplace(client_id, std::make_shared<session>(client_id, endpoint));
    return true; // 新建成功
}

void session_manager::remove_session(const std::string& client_id)
{
    std::unique_lock lock(m_mutex);
    m_sessions_map.erase(client_id);
}

std::vector<asio::ip::udp::endpoint> session_manager::get_active_endpoints()
{
    std::unique_lock lock(m_mutex);
    std::vector<asio::ip::udp::endpoint> endpoints;
    endpoints.reserve(m_sessions_map.size());
    for (const auto& [id, sess_ptr] : m_sessions_map) {
        if (sess_ptr->is_connected() && sess_ptr->is_alive()) {
            endpoints.push_back(sess_ptr->get_endpoint());
        }
    }
    return endpoints;
}

size_t session_manager::get_session_count() const
{
    std::unique_lock lock(m_mutex);
    return m_sessions_map.size();
}

std::optional<std::shared_ptr<session>> session_manager::get_session(const std::string& client_id)
{
    std::unique_lock lock(m_mutex);
    auto it = m_sessions_map.find(client_id);
    if (it != m_sessions_map.end()) {
        return it->second;
    }
    return std::nullopt;
}
void session_manager::update_keepalive(const std::string& client_id)
{
    std::unique_lock lock(m_mutex);
    auto it = m_sessions_map.find(client_id);
    if (it != m_sessions_map.end()) {
        it->second->update_keepalive();
    }
}
void session_manager::check_sessions()
{
    std::unique_lock lock(m_mutex);
    for (auto it = m_sessions_map.begin(); it != m_sessions_map.end();) {
        if (!it->second->is_connected() || !it->second->is_alive()) {
            it = m_sessions_map.erase(it);
        } else {
            ++it;
        }
    }
}
