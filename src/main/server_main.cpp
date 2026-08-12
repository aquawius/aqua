#include "cli_parser_server.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/grpc/grpc_server.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/config.h"
#include "core/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
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

    // ---- WASAPI Loopback Capture（先启动，获取 AudioFormat）----
    auto capture = aqua::audio::create_capture_backend();
    if (!capture) {
        std::cerr << "Error: no audio capture backend available\n";
        return 1;
    }

    aqua::audio::SpscRingBuffer ringbuffer(aqua::config::CAPTURE_RINGBUFFER_SIZE);

    aqua::AudioFormat capture_format{};
    if (!capture->start([&](std::span<const std::byte> pcm) {
            ringbuffer.write(pcm);
        }, capture_format)) {
        std::cerr << "Error: failed to start audio capture\n";
        return 1;
    }

    aqua::log_info_fmt("Capture format: {}ch {}Hz encoding={}",
                       capture_format.channels, capture_format.sample_rate,
                       static_cast<int>(capture_format.encoding));

    // ---- SessionManager ----
    aqua::SessionManager sessions;

    // ---- UDP Transport ----
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind(parsed.bind_ip, parsed.udp_port)) {
        std::cerr << "Error: failed to bind UDP port " << parsed.udp_port << "\n";
        return 1;
    }

    // UDP 接收回调：处理 HELLO / AUDIO
    transport.start_receive([&](const asio::ip::udp::endpoint& sender,
                                std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (!type) return;

        if (*type == aqua::net::PacketType::Hello) {
            auto hello = aqua::net::decode_hello(data);
            if (hello) {
                // 记录 NAT 后的真实 endpoint
                if (sessions.establish_udp(hello->session_id, sender)) {
                    aqua::log_info_fmt("Session 0x{:08X} UDP established: {}:{}",
                                       hello->session_id,
                                       sender.address().to_string(), sender.port());
                    // 回复 HELLO_ACK
                    std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack_buf{};
                    aqua::net::encode_hello_ack(hello->session_id, ack_buf);
                    transport.send(sender,
                                   std::span<const std::byte>{ack_buf.data(), ack_buf.size()});
                } else {
                    aqua::log_warn_fmt("HELLO from unknown session 0x{:08X}",
                                       hello->session_id);
                }
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            // Server 不接收音频（单向），但更新 last_seen
            auto decoded = aqua::net::decode_audio(data);
            if (decoded) {
                sessions.touch_session(decoded->header.session_id);
            }
        }
    });

    std::thread ioc_thread([&] {
        ioc.run();
    });

    // ---- gRPC Server ----
    aqua::grpc::GrpcServer grpc_server(
        sessions, capture_format,
        parsed.bind_ip, parsed.rpc_port,
        parsed.bind_ip, parsed.udp_port);

    std::thread grpc_thread([&] {
        grpc_server.run();
    });

    // ---- Session 超时清理线程 ----
    std::thread cleanup_thread([&] {
        while (g_running) {
            std::this_thread::sleep_for(aqua::config::EXPIRED_CLEANUP_INTERVAL);
            auto expired = sessions.collect_expired_sessions(aqua::config::UDP_SESSION_TIMEOUT);
            for (auto id : expired) {
                aqua::log_info_fmt("Session 0x{:08X} expired, removing", id);
                sessions.remove_session(id);
            }
        }
    });

    // ---- Packetizer Thread ----
    std::thread sender_thread([&] {
        const std::uint32_t frames_per_packet =
            capture_format.sample_rate * aqua::config::AUDIO_PACKET_MS / 1000;
        const std::size_t packet_payload_size =
            frames_per_packet * capture_format.frame_bytes();
        const std::size_t send_buf_size = sizeof(aqua::net::AudioPacketHeader) + packet_payload_size;

        std::vector<std::byte> send_buf(send_buf_size);
        std::vector<std::byte> pcm_buf(packet_payload_size);

        std::uint32_t sequence = 0;
        std::uint64_t sample_position = 0;

        while (g_running) {
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

            // 编码音频包
            auto written = aqua::net::encode_audio(
                0, // session_id=0 表示广播（每个 session 都收到相同数据）
                sequence,
                static_cast<std::uint32_t>(sample_position),
                std::span<const std::byte>{pcm_buf.data(), got},
                std::span<std::byte>{send_buf.data(), send_buf.size()});

            if (written > 0) {
                // 向所有 Connected session 发送
                sessions.for_each_connected([&](auto /*id*/, const auto& endpoint) {
                    transport.send(endpoint,
                                   std::span<const std::byte>{send_buf.data(), written});
                    return true; // 继续遍历
                });
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
    grpc_server.shutdown();
    transport.stop();
    ioc.stop();

    if (grpc_thread.joinable()) grpc_thread.join();
    if (ioc_thread.joinable()) ioc_thread.join();
    if (cleanup_thread.joinable()) cleanup_thread.join();
    if (sender_thread.joinable()) sender_thread.join();

    aqua::log_info("Server stopped.");
    return 0;
}
