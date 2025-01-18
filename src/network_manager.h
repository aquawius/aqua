//
// Created by aquawius on 25-1-9.
//

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <memory>
#include <vector>
#include <deque>
#include <span>
#include <thread>
#include <boost/asio.hpp>

namespace asio = boost::asio;

class network_manager {
public:
    using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;

    /*
     * // 不使用 as_tuple_t
     *      try {
     *          size_t n = co_await socket.async_read_some(buffer, asio::use_awaitable);
     *          // 处理数据
     *      } catch (const std::system_error& e) {
     *          // 处理错误
     *      }
     *
     * // 使用 as_tuple_t
     *      auto [ec, n] = co_await socket.async_read_some(buffer);
     *      if (ec) {
     *          // 处理错误
     *      } else {
     *          // 处理数据
     *      }
     */

    using udp_socket = default_token::as_default_on_t<asio::ip::udp::socket>;
    using steady_timer = default_token::as_default_on_t<asio::steady_timer>;

    /*
     * // 不使用 as_default_on_t
     *      asio::ip::tcp::socket socket(io_context);
     *      auto [ec, n] = co_await socket.async_read_some(buffer, asio::use_awaitable);

     * // 使用 as_default_on_t
     *      tcp_socket socket(io_context);
     *      auto [ec, n] = co_await socket.async_read_some(buffer);  // 更简洁
    */

    static network_manager& get_instance();

    std::string get_default_address();
    std::vector<std::string> network_manager::get_address_list();

    bool init(uint16_t port = 10120);
    void send_audio_data(std::span<const float> audio_data);
    void stop();

private:
    network_manager();
    ~network_manager();
    network_manager(const network_manager&) = delete;
    network_manager& operator=(const network_manager&) = delete;

    asio::awaitable<void> listener_routine();
    asio::awaitable<void> sender_routine();

    std::unique_ptr<asio::io_context> m_io_context;
    std::unique_ptr<udp_socket> m_socket;
    std::jthread m_network_thread;

    std::vector<asio::ip::udp::endpoint> m_clients;
    std::mutex m_clients_mutex;

    std::deque<std::vector<uint8_t>> m_send_queue;
    std::mutex m_queue_mutex;

    std::atomic<bool> m_running { false };
};

#endif // NETWORK_MANAGER_H
