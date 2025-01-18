//
// Created by aquawius on 25-1-18.
//

#pragma once

#include <boost/asio.hpp>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace ip = asio::ip;

class test_client {
public:
    test_client(const std::string& address, uint16_t port);
    void start();

private:
    void start_receive();
    void handle_receive(std::size_t bytes_received);

private:
    asio::io_context m_io_context;
    ip::udp::socket m_socket;
    ip::udp::endpoint m_server_endpoint;
    ip::udp::endpoint m_remote_endpoint;
    std::vector<uint8_t> m_recv_buffer;
};
