//
// Created by aquawius on 25-1-19.
//

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include "session.hpp"

class session_manager {
public:
    static session_manager& get_instance();

    bool add_session(std::string client_id, const asio::ip::udp::endpoint& endpoint);
    void remove_session(const std::string& client_id);
    std::vector<asio::ip::udp::endpoint> get_active_endpoints();
    size_t get_session_count() const;
    std::optional<std::shared_ptr<session>> get_session(const std::string& client_id);
    void update_keepalive(const std::string& client_id);
    void check_sessions(); // 检查并清理超时会话

private:
    session_manager() = default;
    ~session_manager() = default;
    session_manager(const session_manager&) = delete;
    session_manager& operator=(const session_manager&) = delete;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<session>> sessions_;
};

#endif // SESSION_MANAGER_H