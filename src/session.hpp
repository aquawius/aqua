//
// Created by aquawius on 25-1-19.
//

#ifndef SESSION_H
#define SESSION_H

#include <string>
#include <boost/asio.hpp>
#include <chrono>

namespace asio = boost::asio;

class session {
public:
    session(std::string client_id, const asio::ip::udp::endpoint& endpoint)
        : m_client_id(std::move(client_id))
        , m_endpoint(endpoint)
        , m_connected(true)
        , m_last_keepalive(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] const std::string& get_client_id() const { return m_client_id; }
    [[nodiscard]] const asio::ip::udp::endpoint& get_endpoint() const { return m_endpoint; }
    [[nodiscard]] bool is_connected() const { return m_connected; }

    void update_keepalive()
    {
        m_last_keepalive = std::chrono::steady_clock::now();
    }

    [[nodiscard]] bool is_alive() const
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(
                   now - m_last_keepalive)
                   .count()
            < 10; // 10秒超时
    }

    void disconnect() { m_connected = false; }

private:
    std::string m_client_id;
    asio::ip::udp::endpoint m_endpoint;
    std::atomic<bool> m_connected;
    std::chrono::steady_clock::time_point m_last_keepalive;
};

#endif // SESSION_H