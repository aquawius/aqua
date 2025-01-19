//
// Created by aquawius on 25-1-9.
//

// network_manager.hpp
#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <memory>
#include <vector>
#include <deque>
#include <span>
#include <thread>
#include <boost/asio.hpp>
#include <grpcpp/grpcpp.h>
#include "audio_service.grpc.pb.h"
#include "session_manager.h"

namespace asio = boost::asio;

class network_manager {
public:
    using default_token = asio::as_tuple_t<asio::use_awaitable_t<>>;
    using udp_socket = default_token::as_default_on_t<asio::ip::udp::socket>;
    using steady_timer = default_token::as_default_on_t<asio::steady_timer>;

    static network_manager& get_instance();

    // 网络接口管理
    std::string get_default_address();
    std::vector<std::string> get_address_list();

    // 服务器控制
    bool init(const std::string& bind_address = "",
             uint16_t grpc_port = 10120,
             uint16_t udp_port = 10120);
    bool start_server();
    bool stop_server();
    bool is_running() const { return is_running_; }

    // 音频数据处理
    void push_audio_data(std::span<const float> audio_data);

    // 统计信息
    uint64_t get_total_bytes_sent() const { return total_bytes_sent_; }
    uint64_t get_total_bytes_received() const { return total_bytes_received_; }
    size_t get_client_count() const { return session_manager::get_instance().get_session_count(); }

private:
    network_manager();
    ~network_manager();
    network_manager(const network_manager&) = delete;
    network_manager& operator=(const network_manager&) = delete;

    // gRPC 服务实现
    class AudioServiceImpl final : public audio::AudioService::Service {
        grpc::Status Connect(grpc::ServerContext* context,
                           const audio::ConnectRequest* request,
                           audio::ConnectResponse* response) override;

        grpc::Status Disconnect(grpc::ServerContext* context,
                              const audio::DisconnectRequest* request,
                              audio::DisconnectResponse* response) override;

        grpc::Status KeepAlive(grpc::ServerContext* context,
                              const audio::KeepAliveRequest* request,
                              audio::KeepAliveResponse* response) override;
    private:
        network_manager& manager_;
    };

    // 协程处理函数
    asio::awaitable<void> handle_udp_send();
    asio::awaitable<void> check_sessions_routine();

    // 音频数据队列
    struct audio_packet {
        std::vector<uint8_t> data;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::deque<audio_packet> send_queue_;
    std::mutex queue_mutex_;
    static constexpr size_t MAX_QUEUE_SIZE = 1000;

    // IO Context
    std::unique_ptr<asio::io_context> io_context_;
    std::unique_ptr<asio::io_context::work> work_guard_;

    // gRPC 服务器
    std::unique_ptr<grpc::Server> grpc_server_;
    std::unique_ptr<AudioServiceImpl> service_impl_;

    // UDP 套接字
    std::unique_ptr<udp_socket> udp_socket_;

    // 工作线程
    std::vector<std::jthread> io_threads_;     // UDP线程
    std::vector<std::jthread> grpc_threads_;   // gRPC线程

    // 状态标志
    std::atomic<bool> is_running_{false};

    // 统计信息
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
};

#endif // NETWORK_MANAGER_H