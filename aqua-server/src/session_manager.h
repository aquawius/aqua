//
// Created by aquawius on 25-1-19.
//

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <string>

#include <boost/asio.hpp>

#include "session.hpp"

namespace asio = boost::asio;

class session_manager
{
public:
    static session_manager& get_instance();

    bool add_session(const std::string& uuid, const boost::asio::ip::udp::endpoint& endpoint);
    bool remove_session(const std::string& uuid);
    bool update_keepalive(const std::string& uuid);
    bool is_session_valid(const std::string& uuid);
    void check_sessions();
    void clear_all_sessions();

    size_t get_session_count() const;
    std::vector<boost::asio::ip::udp::endpoint> get_active_endpoints() const;
    std::optional<std::shared_ptr<session>> get_session(const std::string& uuid) const;
    std::vector<std::shared_ptr<session>> get_sessions() const;

private:
    session_manager();
    ~session_manager() = default;
    session_manager(const session_manager&) = delete;
    session_manager& operator=(const session_manager&) = delete;

    std::unordered_map<std::string, std::shared_ptr<session>> m_sessions;
    mutable std::shared_mutex m_mutex;

    static constexpr auto SESSION_TIMEOUT = std::chrono::seconds(3);
};

#endif // SESSION_MANAGER_H
