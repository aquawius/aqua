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
        : client_id_(std::move(client_id))
        , endpoint_(endpoint)
        , connected_(true)
        , last_keepalive_(std::chrono::steady_clock::now())
    {
    }

    const std::string& get_client_id() const { return client_id_; }
    const asio::ip::udp::endpoint& get_endpoint() const { return endpoint_; }
    bool is_connected() const { return connected_; }

    void update_keepalive()
    {
        last_keepalive_ = std::chrono::steady_clock::now();
    }

    bool is_alive() const
    {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(
                   now - last_keepalive_)
                   .count()
            < 10; // 10秒超时
    }

    void disconnect() { connected_ = false; }

private:
    std::string client_id_;
    asio::ip::udp::endpoint endpoint_;
    std::atomic<bool> connected_;
    std::chrono::steady_clock::time_point last_keepalive_;
};

#endif // SESSION_H