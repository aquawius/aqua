//
// Created by aquawius on 25-1-9.
//

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <deque>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <grpcpp/grpcpp.h>

#include "rpc_server.h"
#include "session_manager.h"

class RPCServer;
class audio_manager;

namespace asio = boost::asio;

class network_server
{
public:
    using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
    using udp_socket = default_token::as_default_on_t<asio::ip::udp::socket>;
    using steady_timer = default_token::as_default_on_t<asio::steady_timer>;

    using shutdown_callback = std::function<void()>;
    // 异常回调
    void set_shutdown_callback(shutdown_callback cb);

    static std::unique_ptr<network_server> create(
        audio_manager& audio_mgr,
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
    void push_audio_data(std::span<const std::byte> audio_data);

    // 统计信息
    uint64_t get_total_bytes_sent() const { return m_total_bytes_sent; }
    size_t get_client_count() const { return session_manager::get_instance().get_session_count(); }

    // 一些外部获取信息的接口
    std::string get_server_address() const;
    uint16_t get_server_udp_port() const;
    uint16_t get_server_grpc_port() const;

private:
    network_server(audio_manager& audio_mgr);

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

    // 音频数据包相关常量
    static constexpr size_t MTU_SIZE = 1400; // UDP MTU size
    static constexpr size_t AUDIO_HEADER_SIZE = sizeof(uint32_t) + sizeof(uint64_t); // seq + timestamp
    static constexpr size_t MAX_AUDIO_PAYLOAD = MTU_SIZE - AUDIO_HEADER_SIZE;
    static constexpr size_t SAMPLES_PER_PACKET = MAX_AUDIO_PAYLOAD / sizeof(float);

    static constexpr size_t MAX_SEND_QUEUE_SIZE = 300;
    static constexpr size_t MAX_SEND_QUEUE_BATCH_PROCESS_SIZE = 5;

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

    // 音频包头结构
    struct AudioPacketHeader
    {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp; // 时间戳
    }
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((packed))
#endif
    ;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

    static_assert(sizeof(AudioPacketHeader) == sizeof(uint32_t) + sizeof(uint64_t),
                  "AudioPacketHeader Size align ERROR!");

    uint32_t m_sequence_number { 0 }; // 序列号计数器

    std::deque<std::vector<uint8_t>> m_send_queue;
    std::mutex m_queue_mutex;

    // IO Context
    std::unique_ptr<asio::io_context> m_io_context;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> m_work_guard;

    // gRPC 服务器
    std::unique_ptr<grpc::Server> m_grpc_server;

    // UDP 套接字
    std::unique_ptr<udp_socket> m_udp_socket;

    // 工作线程
    std::vector<std::jthread> m_io_threads; // UDP线程
    std::vector<std::jthread> m_grpc_threads; // gRPC线程

    // 状态标志
    std::atomic<bool> m_is_running { false };

    // 异常关闭回调
    // TODO: more shutdown_cb detection.
    shutdown_callback m_shutdown_cb;

    // 统计信息
    std::atomic<uint64_t> m_total_bytes_sent { 0 };

    // 音频管理器引用
    audio_manager& m_audio_manager;

    struct server_config
    {
        std::string server_address;
        uint16_t grpc_port;
        uint16_t udp_port;
    };

    static server_config m_server_config;
};

#endif // NETWORK_MANAGER_H
