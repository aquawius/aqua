//
// Created by aquawius on 25-1-25.
//

#include "udp_test_client.h"

#include <iostream>
#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

udp_client::udp_client(const std::string& address, unsigned short port)
    : socket_(io_context_)
{
    work_guard_ = std::make_unique<boost::asio::io_context::work>(io_context_);

    // 建立本地端点（address:port）
    boost::asio::ip::udp::endpoint local_endpoint(
        boost::asio::ip::make_address(address), port);

    // 打开套接字并绑定
    boost::system::error_code ec;
    socket_.open(local_endpoint.protocol(), ec);
    if (ec) {
        spdlog::error("Failed to open UDP Client Socket: {}", ec.message());
        return;
    }

    socket_.bind(local_endpoint, ec);
    if (ec) {
        spdlog::error("Failed to bind UDP Client Socket at {}:{}: {}",
            address, port, ec.message());
        return;
    }

    spdlog::info("Client bound to {}:{}", address, port);
}

udp_client::~udp_client()
{
    stop();
}

void udp_client::start()
{
    if (running_.exchange(true)) {
        spdlog::warn("Client already running.");
        return;
    }
    spdlog::info("Starting client receive loop...");

    // 启动协程
    boost::asio::co_spawn(io_context_, [this]() { return receive_loop(); }, boost::asio::detached);

    // 在独立线程中运行 io_context
    io_thread_ = std::thread([this]() {
        io_context_.run();
    });
}

void udp_client::stop()
{
    bool expected = true;
    // running_ 切换到 false
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    spdlog::info("Stopping client...");

    // 关闭套接字
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        spdlog::warn("Error closing socket: {}", ec.message());
    }

    // 停止 io_context 并等待线程结束
    io_context_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    spdlog::info("Client stopped.");
}

boost::asio::awaitable<void> udp_client::receive_loop()
{
    using namespace boost::asio;
    using namespace std::literals;

    while (running_) {
        // 异步接收，as_tuple 返回 (error_code, bytes_received)
        auto [ec, bytes_recvd] = co_await socket_.async_receive_from(
            buffer(recv_buffer_), remote_endpoint_,
            as_tuple(use_awaitable));

        if (!ec && bytes_recvd > 0) {
            // 仅演示：输出前16字节
            // std::size_t preview_len = std::min(bytes_recvd, static_cast<std::size_t>(16));
            // spdlog::info("Received {} bytes, show first {}:",
                // bytes_recvd, preview_len);

            // 假设收到的数据是若干 float（只做示例，真实情况需要区分字节序、格式等）
            if (bytes_recvd % sizeof(float) == 0) {
                std::vector<float> float_data(bytes_recvd / sizeof(float));
                std::memcpy(float_data.data(), recv_buffer_.data(), bytes_recvd);

                // 简易计算峰值
                float local_peak = 0.0f;
                for (float val : float_data) {
                    float abs_val = std::fabs(val);
                    if (abs_val > local_peak) {
                        local_peak = abs_val;
                    }
                }

                // 显示简单音量条
                constexpr int meter_width = 50;
                int peak_level = static_cast<int>(local_peak * meter_width);
                peak_level = std::clamp(peak_level, 0, meter_width);

                std::string meter(peak_level, '#');
                meter.resize(meter_width, '-');

                spdlog::info("[volume] [{}] {:.4f}", meter, local_peak);
            }
        } else if (ec) {
            spdlog::error("Receive error: {}", ec.message());
        }
    }

    co_return;
}
