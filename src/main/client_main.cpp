#include "cli_parser_client.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
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
    auto parsed = aqua::parse_client_command_line(argc, argv);

    if (!parsed.success) {
        std::cerr << "Error: " << parsed.error_message << "\n";
        return 1;
    }
    if (parsed.show_help) {
        std::cout << parsed.help_message;
        return 0;
    }
    if (parsed.show_version) {
        std::cout << "aqua_client " << VERSION << "\n";
        return 0;
    }

    aqua::set_log_level(aqua::LogLevel::Info);
    aqua::log_info_fmt("Starting Aqua client, server={}:{} (UDP={})",
                       parsed.server_ip, parsed.server_rpc_port, parsed.server_udp_port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- UDP Transport ----
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind("0.0.0.0", 0)) {
        std::cerr << "Error: failed to bind local UDP port\n";
        return 1;
    }

    auto local_ep = transport.socket_local_endpoint();
    aqua::log_info_fmt("Client UDP bound to {}:{}", local_ep.address().to_string(), local_ep.port());

    // RingBuffer: 网络线程 → 播放线程
    // 128KB 缓冲，约 340ms @ F32LE/48k/2ch
    aqua::audio::SpscRingBuffer ringbuffer(128 * 1024);

    // 解析服务器 endpoint
    asio::ip::udp::endpoint server_endpoint(
        asio::ip::make_address(parsed.server_ip), parsed.server_udp_port);

    // ---- UDP 接收 → RingBuffer ----
    transport.start_receive([&](const asio::ip::udp::endpoint& /*sender*/,
                                std::span<const std::byte> data) {
        auto decoded = aqua::net::decode_audio(data);
        if (decoded) {
            ringbuffer.write(decoded->payload);
        }
    });

    std::thread ioc_thread([&] {
        ioc.run();
    });

    // ---- WASAPI Playback ----
    // M1: 硬编码 F32LE/48k/2ch（Windows 标准 mix format）
    // 后续 M3 通过 gRPC Connect 获取服务器实际格式
    aqua::AudioFormat playback_format{
        aqua::AudioEncoding::PcmF32LE, 2, 48000
    };

    auto playback = aqua::audio::create_playback_backend();
    if (!playback) {
        std::cerr << "Error: no audio playback backend available\n";
        g_running = false;
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        return 1;
    }

    if (!playback->start(playback_format, [&](std::span<std::byte> out) -> std::size_t {
            return ringbuffer.read(out);
        })) {
        std::cerr << "Error: failed to start audio playback\n";
        g_running = false;
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        return 1;
    }

    aqua::log_info("Playback started, waiting for audio data...");

    // ---- 发送 HELLO 到服务器 ----
    std::thread hello_thread([&] {
        std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf{};
        auto written = aqua::net::encode_hello(0, hello_buf);

        // 每 2 秒重发 HELLO，直到收到第一个音频包
        while (g_running) {
            transport.send(server_endpoint,
                           std::span<const std::byte>{hello_buf.data(), written});
            if (ringbuffer.available_read() > 0) {
                break; // 已开始接收音频
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    // 等待退出
    aqua::log_info("Client running. Press Ctrl+C to stop.");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    aqua::log_info("Shutting down...");
    playback->stop();
    g_running = false;
    transport.stop();
    ioc.stop();

    if (ioc_thread.joinable()) ioc_thread.join();
    if (hello_thread.joinable()) hello_thread.join();

    aqua::log_info("Client stopped.");
    return 0;
}
