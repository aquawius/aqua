#include "cli_parser_server.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"

#include <asio.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

constexpr char VERSION[] = "0.0.1";

namespace {
std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running = false;
}
} // namespace

int main(int argc, char** argv)
{
    auto parsed = aqua::parse_server_command_line(argc, argv);

    if (!parsed.success) {
        std::cerr << "Error: " << parsed.error_message << "\n";
        return 1;
    }
    if (parsed.show_help) {
        std::cout << parsed.help_message;
        return 0;
    }
    if (parsed.show_version) {
        std::cout << "aqua_server " << VERSION << "\n";
        return 0;
    }

    aqua::set_log_level(aqua::LogLevel::Info);
    aqua::log_info_fmt("Starting Aqua server on {} gRPC={}, UDP={}",
                       parsed.bind_ip, parsed.rpc_port, parsed.udp_port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- UDP Transport ----
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind(parsed.bind_ip, parsed.udp_port)) {
        std::cerr << "Error: failed to bind UDP port " << parsed.udp_port << "\n";
        return 1;
    }

    // 记录最后发送 HELLO 的客户端 endpoint
    std::mutex client_mutex;
    std::optional<asio::ip::udp::endpoint> client_endpoint;
    std::atomic<bool> client_connected{false};

    transport.start_receive([&](const asio::ip::udp::endpoint& sender,
                                std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (type == aqua::net::PacketType::Hello) {
            std::lock_guard lock(client_mutex);
            client_endpoint = sender;
            client_connected = true;
            aqua::log_info_fmt("Client connected: {}:{}", sender.address().to_string(), sender.port());
        }
    });

    std::thread ioc_thread([&] {
        ioc.run();
    });

    // ---- WASAPI Loopback Capture ----
    auto capture = aqua::audio::create_capture_backend();
    if (!capture) {
        std::cerr << "Error: no audio capture backend available\n";
        g_running = false;
        ioc.stop();
        ioc_thread.join();
        return 1;
    }

    // RingBuffer: 音频线程 → 网络线程
    // 64KB 缓冲，约 170ms @ F32LE/48k/2ch
    aqua::audio::SpscRingBuffer ringbuffer(64 * 1024);

    aqua::AudioFormat capture_format{};
    if (!capture->start([&](std::span<const std::byte> pcm) {
            ringbuffer.write(pcm);
        }, capture_format)) {
        std::cerr << "Error: failed to start audio capture\n";
        g_running = false;
        ioc.stop();
        ioc_thread.join();
        return 1;
    }

    aqua::log_info_fmt("Capture format: {}ch {}Hz encoding={}",
                       capture_format.channels, capture_format.sample_rate,
                       static_cast<int>(capture_format.encoding));

    // ---- Packetizer Thread ----
    // 从 RingBuffer 读取 PCM，分片为 AudioPacket，通过 UDP 发送
    std::thread sender_thread([&] {
        // 每包 10ms 音频
        const std::uint32_t frames_per_packet =
            capture_format.sample_rate / 100; // 10ms
        const std::size_t packet_payload_size =
            frames_per_packet * capture_format.frame_bytes();
        const std::size_t send_buf_size = sizeof(aqua::net::AudioPacketHeader) + packet_payload_size;

        std::vector<std::byte> send_buf(send_buf_size);
        std::vector<std::byte> pcm_buf(packet_payload_size);

        std::uint32_t sequence = 0;
        std::uint64_t sample_position = 0;

        while (g_running) {
            // 等待客户端连接
            if (!client_connected.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // 从 RingBuffer 读取一包数据
            std::size_t got = 0;
            while (got < pcm_buf.size() && g_running) {
                got += ringbuffer.read(std::span<std::byte>{
                    pcm_buf.data() + got, pcm_buf.size() - got});
                if (got < pcm_buf.size()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (!g_running) break;

            // 编码并发送
            auto written = aqua::net::encode_audio(
                0, // session_id=0 for M1 (no session management)
                sequence,
                static_cast<std::uint32_t>(sample_position),
                std::span<const std::byte>{pcm_buf.data(), got},
                std::span<std::byte>{send_buf.data(), send_buf.size()});

            if (written > 0) {
                std::lock_guard lock(client_mutex);
                if (client_endpoint) {
                    transport.send(*client_endpoint,
                                   std::span<const std::byte>{send_buf.data(), written});
                }
            }

            sequence++;
            sample_position += frames_per_packet;
        }
    });

    // 等待退出
    aqua::log_info("Server running. Press Ctrl+C to stop.");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    aqua::log_info("Shutting down...");
    capture->stop();
    g_running = false;
    transport.stop();
    ioc.stop();

    if (ioc_thread.joinable()) ioc_thread.join();
    if (sender_thread.joinable()) sender_thread.join();

    aqua::log_info("Server stopped.");
    return 0;
}
