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
#include <boost/asio/executor_work_guard.hpp>
#include <grpcpp/grpcpp.h>

#include "rpc_server.h"
#include "session_manager.h"

class RPCServer;
namespace asio = boost::asio;

class network_server {
public:
    using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
    using udp_socket = default_token::as_default_on_t<asio::ip::udp::socket>;
    using steady_timer = default_token::as_default_on_t<asio::steady_timer>;

    static std::unique_ptr<network_server> create(
        const std::string& bind_address = "",
        uint16_t grpc_port = 10120,
        uint16_t udp_port = 10120);

    bool start_server();
    bool stop_server();
    bool is_running() const { return m_is_running; }

    // 网络接口管理
    static std::string get_default_address();
    static std::vector<std::string> get_address_list();

    // 音频数据处理
    void push_audio_data(std::span<const float> audio_data);

    // 统计信息
    uint64_t get_total_bytes_sent() const { return m_total_bytes_sent; }
    uint64_t get_total_bytes_received() const { return m_total_bytes_received; }
    size_t get_client_count() const { return session_manager::get_instance().get_session_count(); }

private:
    network_server();

public:
    ~network_server();
    network_server(const network_server&) = delete;
    network_server& operator=(const network_server&) = delete;

private:
    // 内部资源的申请和释放
    bool init_resources(const std::string& addr, uint16_t grpc_port, uint16_t udp_port);
    void release_resources();

    // 协程处理函数
    asio::awaitable<void> handle_udp_send();
    asio::awaitable<void> check_sessions_routine();

    // 音频数据队列
    struct audio_packet {
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::deque<audio_packet> m_send_queue;
    std::mutex m_queue_mutex;
    static constexpr size_t MAX_SEND_QUEUE_SIZE = 100;

    // IO Context
    std::unique_ptr<asio::io_context> m_io_context;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> m_work_guard;

    // gRPC 服务器
    std::unique_ptr<grpc::Server> m_grpc_server;
    std::unique_ptr<RPCServer> m_rpc_server; // 业务逻辑层可在此注册

    // UDP 套接字
    std::unique_ptr<udp_socket> m_udp_socket;

    // 工作线程
    std::vector<std::jthread> m_io_threads; // UDP线程
    std::vector<std::jthread> m_grpc_threads; // gRPC线程

    // 状态标志
    std::atomic<bool> m_is_running { false };

    // 统计信息
    std::atomic<uint64_t> m_total_bytes_sent { 0 };
    std::atomic<uint64_t> m_total_bytes_received { 0 };
};

#endif // NETWORK_MANAGER_H
