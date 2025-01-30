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

    // 音频数据包相关常量
    static constexpr size_t MTU_SIZE = 1400; // UDP MTU size
    static constexpr size_t AUDIO_HEADER_SIZE = sizeof(uint32_t) + sizeof(uint64_t); // seq + timestamp
    static constexpr size_t MAX_AUDIO_PAYLOAD = MTU_SIZE - AUDIO_HEADER_SIZE;
    static constexpr size_t SAMPLES_PER_PACKET = MAX_AUDIO_PAYLOAD / sizeof(float);

    static constexpr std::chrono::milliseconds KEEPALIVE_INTERVAL = 1000ms;
    static constexpr std::chrono::milliseconds AUDIO_PROCESS_INTERVAL = 1ms;


    explicit network_client(client_config cfg);
    ~network_client();

    // 启动和停止客户端
    bool start_client();
    bool stop_client();
    bool is_running() const { return m_running; }

    // 状态查询
    uint64_t get_total_bytes_received() const { return m_total_bytes_received; }
    bool is_connected() const { return !m_client_uuid.empty(); }
    const audio_playback_linux::stream_config& get_audio_config() const;

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
    void process_complete_audio();

    // 配置
    client_config m_cfg;
    audio_playback_linux::stream_config audio_config;

    // 音频播放
    std::unique_ptr<audio_playback_linux> m_audio_playback;

    // RPC客户端
    std::string m_client_uuid;
    std::unique_ptr<rpc_client> m_rpc_client;

    // 异步IO资源
    std::jthread m_io_thread;
    boost::asio::io_context m_io_context;
    std::unique_ptr<boost::asio::io_context::work> m_work_guard;
    boost::asio::ip::udp::socket m_udp_socket;

    // 状态控制
    std::atomic<bool> m_running { false };
    std::atomic<uint64_t> m_total_bytes_received { 0 };

    // 接收缓冲区
    std::vector<uint8_t> m_recv_buffer {};

    // 音频包头结构
    struct AudioPacketHeader {
        uint32_t sequence_number; // 序列号
        uint64_t timestamp; // 时间戳
    } __attribute__((packed));
    uint32_t m_sequence_number { 0 };

    // 音频包重组缓冲区
    struct AudioBuffer {
        std::vector<float> samples;
        uint32_t expected_sequence{0};  // 期望的下一个序列号
        static constexpr size_t MAX_BUFFER_SIZE = 1024;  // 缓冲区最大样本数

        void reset() {
            samples.clear();
            expected_sequence = 0;
        }
    };
    AudioBuffer m_audio_buffer;
};

#endif // NETWORK_CLIENT_H