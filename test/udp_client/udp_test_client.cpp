//
// Created by aquawius on 25-1-25.
//

#include "udp_test_client.h"

#include <spdlog/spdlog.h>
#include <iostream>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

udp_client::udp_client(const std::string& listen_address, unsigned short port)
    : socket_(io_context_)
{
    // 为io_context_创建一个work_guard_，防止其立即退出
    work_guard_ = std::make_unique<boost::asio::io_context::work>(io_context_);

    // 打开并绑定UDP Socket
    boost::asio::ip::udp::endpoint local_endpoint(
        boost::asio::ip::make_address(listen_address), port);

    boost::system::error_code ec;
    socket_.open(local_endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("Failed to open UDP Client Socket: {}", ec.message());
        return;
    }

    socket_.bind(local_endpoint, ec);
    if (ec) {
        spdlog::error("Failed to bind UDP Client Socket ({}:{}): {}",
                      listen_address, port, ec.message());
        return;
    }
    spdlog::info("UDP Client successfully bound to {}:{}", listen_address, port);
}

udp_client::~udp_client()
{
    stop();
}

void udp_client::start()
{
    // 如果已经启动，则发出警告
    if (running_.exchange(true)) {
        spdlog::warn("UDP Client is already running.");
        return;
    }
    spdlog::info("Starting UDP Client receiving loop...");

    // 启动协程来接收数据
    boost::asio::co_spawn(io_context_,
                          [this]() { return receive_loop(); },
                          boost::asio::detached);

    // 在独立线程中运行io_context_
    io_thread_ = std::thread([this]() {
        io_context_.run();
    });
}

void udp_client::stop()
{
    bool expected = true;
    // 如果此前已经停止，则无需再次停止
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    spdlog::info("Stopping UDP Client...");

    // 关闭socket
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        spdlog::warn("Error closing UDP Client Socket: {}", ec.message());
    }

    // 停止io_context_
    io_context_.stop();

    // 等待线程结束
    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    spdlog::info("UDP Client stopped.");
}

boost::asio::awaitable<void> udp_client::receive_loop()
{
    using namespace boost::asio;

    while (running_) {
        // 使用as_tuple，获取error_code和bytes接收字节数
        auto [ec, bytes_recvd] = co_await socket_.async_receive_from(
            buffer(recv_buffer_), remote_endpoint_,
            as_tuple(use_awaitable));

        if (!ec && bytes_recvd > 0) {
            std::size_t preview_len = std::min(bytes_recvd, static_cast<std::size_t>(16));
            // 打印前若干字节，显示已收到数据
            spdlog::info("Received data from {}:{} (first {} bytes):",
                         remote_endpoint_.address().to_string(),
                         remote_endpoint_.port(),
                         preview_len);

            // 将前若干字节以十六进制形式打印到stdout
            for (std::size_t i = 0; i < preview_len; ++i) {
                std::cout << std::hex << (0xFF & recv_buffer_[i]) << " ";
            }
            std::cout << std::dec << std::endl;
        } else {
            if (ec) {
                spdlog::error("UDP receive error: {}", ec.message());
            }
        }
    }

    co_return;
}
