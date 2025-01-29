//
// Created by aquawius on 25-1-29.
//

// network_client.h
#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include <memory>
#include <span>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#include "rpc_client.h"

using namespace std::chrono_literals;

class network_client {
public:
    struct client_config {
        std::string server_address;
        uint16_t server_port;

        std::string client_address;
        uint16_t client_port;
    };

    static constexpr size_t MAX_RECV_BUFFER_SIZE = 1024;
    static constexpr std::chrono::milliseconds KEEPALIVE_INTERVAL = 1000ms;

    explicit network_client(client_config cfg);
    ~network_client();

    bool connect();
    void disconnect();
    bool start();
    void stop();

private:
    client_config m_cfg;

    // 接收数据协程
    boost::asio::awaitable<void> udp_receive_loop();
    boost::asio::awaitable<void> keepalive_loop();

    bool setup_udp_socket();
    bool setup_rpc_client();

    // rpc_client recived.
    std::string m_client_uuid;

    // 异步资源
    std::jthread m_io_thread;
    boost::asio::io_context m_io_context;
    std::unique_ptr<boost::asio::io_context::work> m_work_guard;
    boost::asio::ip::udp::socket m_udp_socket;
    std::unique_ptr<rpc_client> m_rpc_client;

    // 状态控制
    std::atomic<bool> m_running { false };

    // Buffer
    std::vector<uint8_t> m_recv_buffer; // 动态缓冲区
};

#endif // NETWORK_CLIENT_H