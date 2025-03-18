//
// Created by aquawius on 25-1-19.
//

#ifndef SESSION_H
#define SESSION_H

#include <string>
#include <boost/asio.hpp>
#include <chrono>
#include <utility>

namespace asio = boost::asio;

constexpr auto SESSION_EXPR_TIMEOUT = std::chrono::seconds(3);

class session {
public:
    session(std::string client_id, asio::ip::udp::endpoint endpoint)
        : m_client_uuid(std::move(client_id))
          , m_endpoint(std::move(endpoint))
          , m_connected(true)
          , m_last_keepalive(std::chrono::system_clock::now()) {}

    [[nodiscard]] const std::string& get_client_uuid() const { return m_client_uuid; }
    [[nodiscard]] const asio::ip::udp::endpoint& get_endpoint() const { return m_endpoint; }
    [[nodiscard]] bool is_connected() const { return m_connected; }

    void update_keepalive()
    {
        m_last_keepalive = std::chrono::system_clock::now();
    }

    [[nodiscard]] bool is_alive() const
    {
        const auto now = std::chrono::system_clock::now();
        return (now - m_last_keepalive) < SESSION_EXPR_TIMEOUT;
    }

    void disconnect() { m_connected = false; }

private:
    std::string m_client_uuid;
    asio::ip::udp::endpoint m_endpoint;
    std::atomic<bool> m_connected;
    std::chrono::system_clock::time_point m_last_keepalive;
};

#endif // SESSION_H