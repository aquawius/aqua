//
// Created by aquawius on 25-1-9.
//

#include "network_manager.h"
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

namespace asio = boost::asio;
namespace ip = asio::ip;
using namespace std::chrono_literals;

network_manager& network_manager::get_instance()
{
    static network_manager instance;
    return instance;
}

std::vector<std::string> network_manager::get_address_list()
{

    return {};
}

std::string network_manager::get_default_address()
{
    return {};
}

network_manager::network_manager() = default;

network_manager::~network_manager()
{
    stop();
}

bool network_manager::init(uint16_t port)
{
    try {
        m_io_context = std::make_unique<asio::io_context>();
        m_socket = std::make_unique<udp_socket>(*m_io_context,
            ip::udp::endpoint(ip::udp::v4(), port));

        m_running = true;

        m_network_thread = std::jthread([this] {
            asio::co_spawn(*m_io_context,
                listener_routine(),
                asio::detached);

            asio::co_spawn(*m_io_context,
                sender_routine(),
                asio::detached);

            m_io_context->run();
        });

        spdlog::info("Network manager initialized on port {}", port);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Network initialization failed: {}", e.what());
        return false;
    }
}

void network_manager::stop()
{
    if (!m_running)
        return;

    m_running = false;

    if (m_socket) {
        m_socket->close();
    }

    if (m_io_context) {
        m_io_context->stop();
    }

    if (m_network_thread.joinable()) {
        m_network_thread.join();
    }

    spdlog::info("Network manager stopped");
}

void network_manager::send_audio_data(const std::span<const float> audio_data)
{
    if (!m_running || audio_data.empty())
        return;

    try {
        std::vector<uint8_t> packet;
        packet.reserve(sizeof(float) * audio_data.size() + sizeof(uint32_t));

        uint32_t data_size = audio_data.size();
        const uint8_t* size_ptr = reinterpret_cast<const uint8_t*>(&data_size);
        packet.insert(packet.end(), size_ptr, size_ptr + sizeof(uint32_t));

        const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(audio_data.data());
        packet.insert(packet.end(), data_ptr,
            data_ptr + (audio_data.size() * sizeof(float)));

        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_send_queue.push_back(std::move(packet));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error preparing audio data: {}", e.what());
    }
}

asio::awaitable<void> network_manager::listener_routine()
{
    std::vector<uint8_t> recv_buffer(65536);

    try {
        while (m_running) {
            ip::udp::endpoint client_endpoint;

            auto [ec, bytes] = co_await m_socket->async_receive_from(
                asio::buffer(recv_buffer), client_endpoint);

            if (ec) {
                if (m_running) {
                    spdlog::error("Receive error: {}", ec.message());
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(m_clients_mutex);
                if (std::find(m_clients.begin(), m_clients.end(),
                        client_endpoint)
                    == m_clients.end()) {
                    m_clients.push_back(client_endpoint);
                    spdlog::info("New client connected: {}:{}",
                        client_endpoint.address().to_string(),
                        client_endpoint.port());
                }
            }
        }
    } catch (const std::exception& e) {
        if (m_running) {
            spdlog::error("Listener error: {}", e.what());
        }
    }
}

asio::awaitable<void> network_manager::sender_routine()
{
    try {
        steady_timer timer(*m_io_context);

        while (m_running) {
            std::vector<uint8_t> data_to_send;

            {
                std::lock_guard<std::mutex> lock(m_queue_mutex);
                if (!m_send_queue.empty()) {
                    data_to_send = std::move(m_send_queue.front());
                    m_send_queue.pop_front();
                }
            }

            if (!data_to_send.empty()) {
                std::vector<ip::udp::endpoint> clients;
                {
                    std::lock_guard<std::mutex> lock(m_clients_mutex);
                    clients = m_clients;
                }

                for (const auto& client : clients) {
                    auto [ec, bytes_sent] = co_await m_socket->async_send_to(
                        asio::buffer(data_to_send), client);

                    if (ec && m_running) {
                        spdlog::error("Send error to {}: {}",
                            client.address().to_string(), ec.message());
                    }
                }
            }

            timer.expires_after(1ms);
            auto [ec] = co_await timer.async_wait();
            if (ec && m_running) {
                spdlog::error("Timer error: {}", ec.message());
            }
        }
    } catch (const std::exception& e) {
        if (m_running) {
            spdlog::error("Sender error: {}", e.what());
        }
    }
}
