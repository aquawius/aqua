//
// Created by aquawius on 25-1-18.
//

#include "test_client.h"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>
#include <iostream>

using namespace std::chrono_literals;

test_client::test_client(const std::string& address, uint16_t port)
    : m_io_context()
    , m_socket(m_io_context, ip::udp::endpoint(ip::udp::v4(), 0))
    , m_server_endpoint(ip::address::from_string(address), port)
{
    spdlog::info("Client initialized, connecting to {}:{}", address, port);
}

void test_client::start()
{
    try {
        // 发送初始包以注册客户端
        std::string init_msg = "hello";
        m_socket.send_to(asio::buffer(init_msg), m_server_endpoint);
        spdlog::info("Sent initial message to server");

        start_receive();
        m_io_context.run();
    } catch (const std::exception& e) {
        spdlog::error("Client error: {}", e.what());
    }
}

void test_client::start_receive()
{
    m_recv_buffer.resize(65536);
    m_socket.async_receive_from(
        asio::buffer(m_recv_buffer), m_remote_endpoint,
        [this](const boost::system::error_code& error, std::size_t bytes_received) {
            if (!error) {
                handle_receive(bytes_received);
                start_receive();
            } else {
                spdlog::error("Receive error: {}", error.message());
            }
        });
}

void test_client::handle_receive(std::size_t bytes_received)
{
    if (bytes_received < sizeof(uint32_t)) {
        spdlog::warn("Received packet too small");
        return;
    }

    uint32_t data_size;
    std::memcpy(&data_size, m_recv_buffer.data(), sizeof(uint32_t));

    size_t float_data_size = (bytes_received - sizeof(uint32_t)) / sizeof(float);
    if (float_data_size != data_size) {
        spdlog::warn("Data size mismatch: expected {}, got {}", data_size, float_data_size);
        return;
    }

    const float* audio_data = reinterpret_cast<const float*>(m_recv_buffer.data() + sizeof(uint32_t));
    size_t samples_to_print = std::min(size_t(10), float_data_size);

    std::string samples_str;
    for (size_t i = 0; i < samples_to_print; ++i) {
        samples_str += fmt::format("{:.6f} ", audio_data[i]);
    }

    spdlog::info("Received {} samples. First {} samples: {}",
        float_data_size, samples_to_print, samples_str);
}

int main(int argc, char* argv[])
{
    try {
        cxxopts::Options options("test_client", "Test client for audio streaming");
        options.add_options()
            ("a,address", "Server address", cxxopts::value<std::string>()->default_value("127.0.0.1"))
            ("p,port", "Server port", cxxopts::value<uint16_t>()->default_value("10120"))
            ("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        std::string address = result["address"].as<std::string>();
        uint16_t port = result["port"].as<uint16_t>();

        spdlog::info("Starting client with server address: {}:{}", address, port);

        test_client client(address, port);
        client.start();
    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }

    return 0;
}

