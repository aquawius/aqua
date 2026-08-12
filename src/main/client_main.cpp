#include "cli_parser_client.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/grpc/grpc_client.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/config.h"

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

    aqua::set_log_level(aqua::default_log_level());
    aqua::log_info_fmt("Starting Aqua client, server={}:{}", parsed.server_ip, parsed.server_rpc_port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- gRPC Connect ----
    aqua::grpc::GrpcClient grpc_client;
    if (!grpc_client.connect_to_server(parsed.server_ip, parsed.server_rpc_port)) {
        std::cerr << "Error: failed to connect to gRPC server\n";
        return 1;
    }

    aqua::grpc::ConnectResult connect_result;
    if (!grpc_client.connect("aqua_client", connect_result)) {
        std::cerr << "Error: gRPC Connect failed\n";
        return 1;
    }

    const auto session_id = connect_result.session_id;
    const auto& server_audio_format = connect_result.audio_format;

    if (!server_audio_format.valid()) {
        std::cerr << "Error: server returned invalid audio format\n";
        grpc_client.disconnect(session_id);
        return 1;
    }

    aqua::log_info_fmt("Server audio format: {}ch {}Hz encoding={}",
                       server_audio_format.channels, server_audio_format.sample_rate,
                       static_cast<int>(server_audio_format.encoding));

    // ---- UDP Transport ----
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind("0.0.0.0", 0)) {
        std::cerr << "Error: failed to bind local UDP port\n";
        grpc_client.disconnect(session_id);
        return 1;
    }

    auto local_ep = transport.socket_local_endpoint();
    aqua::log_info_fmt("Client UDP bound to {}:{}", local_ep.address().to_string(), local_ep.port());

    // 服务器 UDP endpoint
    // NAT 设计约定（AGENT.md §6）：client 使用 gRPC server IP + gRPC 返回的 UDP 端口。
    // server 通过 Connect 仅告知 UDP 端口；UDP 目标地址 = gRPC 连接的 server IP。
    // (connect_result.udp_address 为信息性字段，server 绑定 0.0.0.0 时无意义，故忽略。)
    asio::ip::udp::endpoint server_udp_endpoint(
        asio::ip::make_address(parsed.server_ip), connect_result.udp_port);

    // RingBuffer: 网络线程 → 播放线程
    aqua::audio::SpscRingBuffer ringbuffer(aqua::config::PLAYBACK_RINGBUFFER_SIZE);

    // UDP 握手状态
    std::atomic<bool> hello_acked{false};

    // ---- UDP 接收回调 ----
    transport.start_receive([&](const asio::ip::udp::endpoint& /*sender*/,
                                std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (!type) return;

        if (*type == aqua::net::PacketType::HelloAck) {
            auto ack = aqua::net::decode_hello(data);
            if (ack && ack->session_id == session_id) {
                bool was_acked = hello_acked.exchange(true, std::memory_order_relaxed);
                if (!was_acked) {
                    aqua::log_info("UDP HELLO_ACK received, channel established");
                }
                // 后续 HELLO_ACK 是保活响应，静默
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            auto decoded = aqua::net::decode_audio(data);
            if (decoded) {
                ringbuffer.write(decoded->payload);
            }
        }
    });

    std::thread ioc_thread([&] {
        ioc.run();
    });

    // ---- 发送 HELLO 直到收到 HELLO_ACK ----
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf{};
    auto hello_written = aqua::net::encode_hello(session_id, hello_buf);

    while (g_running && !hello_acked.load(std::memory_order_relaxed)) {
        transport.send(server_udp_endpoint,
                       std::span<const std::byte>{hello_buf.data(), hello_written});
        std::this_thread::sleep_for(aqua::config::HELLO_RETRY_INTERVAL);
    }

    if (!hello_acked.load()) {
        std::cerr << "Error: UDP HELLO_ACK timeout\n";
        g_running = false;
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return 1;
    }

    // ---- WASAPI Playback（使用 server 返回的格式）----
    auto playback = aqua::audio::create_playback_backend();
    if (!playback) {
        std::cerr << "Error: no audio playback backend available\n";
        g_running = false;
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return 1;
    }

    if (!playback->start(server_audio_format, [&](std::span<std::byte> out) -> std::size_t {
            return ringbuffer.read(out);
        })) {
        // start() 现在会同步等待 WASAPI 初始化结果；失败时具体 HRESULT 已由
        // wasapi_playback 的 playback_loop 日志输出（如 0x88890008 = 设备被占用）。
        std::cerr << "Error: failed to start audio playback (see log above for details)\n";
        g_running = false;
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return 1;
    }

    aqua::log_info("Playback started with server audio format");

    // ---- UDP HELLO 保活线程 ----
    // 单路保活：UDP HELLO 同时承担两种角色：
    //   1. 刷新 NAT 映射表（数据面），防止路由器因无上行流量清除映射。
    //      某些对称 NAT 要求双向流量才保活，仅靠 server→client 的下行音频不够。
    //   2. 刷新 server 侧 session 的 last_seen（server 收到 HELLO 后
    //      establish_udp → touch_session，幂等）。
    // gRPC 不参与保活，仅负责 Connect / Disconnect 生命周期管理。
    std::thread hello_keepalive_thread([&] {
        while (g_running) {
            std::this_thread::sleep_for(aqua::config::KEEPALIVE_INTERVAL);
            if (!g_running) break;

            // 重发 HELLO 刷新 NAT 映射 + server session last_seen
            transport.send(server_udp_endpoint,
                           std::span<const std::byte>{hello_buf.data(), hello_written});
        }
    });

    // 等待退出
    aqua::log_info("Client running. Press Ctrl+C to stop.");
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // 监控播放后端健康状态：初始化成功后若线程因运行时错误退出
        // （如设备被占用/移除），触发优雅退出，避免客户端空转且持续发送无意义 HELLO 保活。
        if (!playback->is_running()) {
            aqua::log_error("Playback backend stopped unexpectedly, shutting down");
            g_running = false;
        }
    }

    aqua::log_info("Shutting down...");
    playback->stop();
    g_running = false;
    transport.stop();
    ioc.stop();

    if (ioc_thread.joinable()) ioc_thread.join();
    if (hello_keepalive_thread.joinable()) hello_keepalive_thread.join();

    // 优雅断开
    grpc_client.disconnect(session_id);

    aqua::log_info("Client stopped.");
    return 0;
}
