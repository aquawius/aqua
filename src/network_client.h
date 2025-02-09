//
// Created by aquawius on 25-1-29.
//

// network_client.h
#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include "linux/audio_playback_linux.h"

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <thread>

#include "rpc_client.h"
#include <boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/endian/conversion.hpp>

using namespace std::chrono_literals;

class network_client {
public:
    struct client_config {
        std::string server_address;
        uint16_t server_port;
        std::string client_address;
        uint16_t client_port;
    };

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

    // 音频包头结构
    struct AudioPacketHeader {
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

    static_assert(sizeof(AudioPacketHeader) == sizeof(uint32_t) + sizeof(uint64_t), "AudioPacketHeader Size align ERROR!");

    using shutdown_callback = std::function<void()>;

    // 音频数据包相关常量
    static constexpr size_t RECV_BUFFER_SIZE = 1500;
    static constexpr size_t AUDIO_HEADER_SIZE = sizeof(AudioPacketHeader); // seq + timestamp

    static constexpr std::chrono::milliseconds KEEPALIVE_INTERVAL = 1000ms;

    explicit network_client(client_config cfg);
    ~network_client();

    // 异常关闭回调
    void set_shutdown_callback(shutdown_callback cb);

    // 网络接口管理
    static std::string get_default_address();
    static std::vector<std::string> get_address_list();

    // 启动和停止客户端
    bool start_client();
    bool stop_client();
    bool is_running() const { return m_running; }

    // 状态查询
    [[nodiscard]] uint64_t get_total_bytes_received() const;
    [[nodiscard]] bool is_connected() const;

    // TODO: next version, configured stream_config
    [[nodiscard]] const audio_playback_linux::stream_config& get_audio_config() const;

private:
    // 初始化和释放资源
    bool init_resources();
    void release_resources();

    // 网络连接管理
    bool setup_network();
    bool connect_to_server();
    void disconnect_from_server();

    // 协程
    boost::asio::awaitable<void> udp_receive_loop();
    boost::asio::awaitable<void> keepalive_loop();

    // 音频处理
    bool setup_audio();
    void process_received_audio_data(const std::vector<uint8_t>& data_with_header);

    // 配置
    client_config m_client_config;
    audio_playback_linux::stream_config audio_config;

    // 音频播放
    std::unique_ptr<audio_playback_linux> m_audio_playback;

    // RPC客户端
    std::string m_client_uuid;
    std::unique_ptr<rpc_client> m_rpc_client;

    // 异步IO资源
    std::jthread m_io_thread;
    boost::asio::io_context m_io_context;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_work_guard;
    boost::asio::ip::udp::socket m_udp_socket;
    // 接收缓冲区
    std::vector<uint8_t> m_recv_buffer { };

    // 状态控制
    std::atomic<bool> m_running { false };
    std::atomic<uint64_t> m_total_bytes_received { 0 };

    // 异常关闭回调
    shutdown_callback m_shutdown_cb;
};

#endif // NETWORK_CLIENT_H