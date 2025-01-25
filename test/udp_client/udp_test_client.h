//
// Created by aquawius on 25-1-25.
//

#ifndef UDP_TEST_CLIENT_H
#define UDP_TEST_CLIENT_H


#include <thread>
#include <atomic>
#include <array>
#include <memory>
#include <boost/asio.hpp>

class udp_client {
public:
    udp_client(const std::string& listen_address, unsigned short port);
    ~udp_client();

    void start();
    void stop();

private:
    // 接收数据协程
    boost::asio::awaitable<void> receive_loop();

private:
    // io_context 和 work thread
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::io_context::work> work_guard_;
    std::thread io_thread_;

    // UDP socket
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;

    // 接收 buffer
    std::array<char, 1024> recv_buffer_{};

    std::atomic<bool> running_{false};
};


#endif //UDP_TEST_CLIENT_H
