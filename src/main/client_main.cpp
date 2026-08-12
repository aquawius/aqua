#include "cli_parser_client.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/grpc/grpc_client.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/config.h"

#include <asio.hpp>

#include <atomic>
#include <csignal>
#include <functional>
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

    // RingBuffer: JitterBuffer → 播放线程
    aqua::audio::SpscRingBuffer ringbuffer(aqua::config::PLAYBACK_RINGBUFFER_SIZE);

    // 每包 PCM 参数
    const std::uint32_t frames_per_packet =
        server_audio_format.sample_rate * aqua::config::AUDIO_PACKET_MS / 1000;
    const std::size_t packet_payload_size =
        static_cast<std::size_t>(frames_per_packet) * server_audio_format.frame_bytes();

    // JitterBuffer: packet 时间顺序 + jitter + loss
    // push 和 pop_next 都在 io_context 线程执行，无需锁
    aqua::jitter::JitterBuffer jitter_buffer(
        server_audio_format,
        frames_per_packet,
        aqua::config::JITTER_TARGET_LATENCY_PACKETS,
        aqua::config::JITTER_CAPACITY_PACKETS);

    // UDP 握手状态
    std::atomic<bool> hello_acked{false};

    // 接收统计（在 io_context 线程更新，主线程读取；非精确计数但足够用于调试日志）
    std::atomic<std::uint64_t> recv_audio_packets{0};
    std::atomic<std::uint64_t> recv_audio_bytes{0};
    std::atomic<std::uint64_t> recv_hello_acks{0};

    // 最后一次收到 Audio 包的时间（steady_clock 纳秒数）。
    // 主循环据此检测 server 是否已断开：超过 CLIENT_AUDIO_TIMEOUT 未收到数据则退出。
    // 初始化为启动时间，握手期间也算"无数据"，避免握手失败时 client 永不退出。
    std::atomic<int64_t> last_audio_recv_ns{
        std::chrono::steady_clock::now().time_since_epoch().count()};

    // ---- JitterBuffer → RingBuffer 调度器 ----
    // steady_timer 在 io_context 线程中驱动 JitterBuffer 的 pop_next → ringbuffer.write。
    // timer 不是 JitterBuffer 的一部分，只是外部调度手段。
    asio::steady_timer jb_timer(ioc);
    std::vector<std::byte> jb_pop_buf(packet_payload_size);

    std::function<void()> schedule_jb_pop;
    schedule_jb_pop = [&]() {
        auto deadline = jitter_buffer.next_playout_deadline();
        if (!deadline) {
            // 初始缓冲未满，10ms 后重新检查
            jb_timer.expires_after(std::chrono::milliseconds(10));
        } else {
            jb_timer.expires_at(*deadline);
        }

        jb_timer.async_wait([&](const asio::error_code& ec) {
            if (ec || !g_running) return;

            // pop_next → ringbuffer.write
            // 循环 pop 直到 RingBuffer 写满或 deadline 未到
            while (g_running) {
                auto dl = jitter_buffer.next_playout_deadline();
                if (!dl) break;

                auto now = std::chrono::steady_clock::now();
                if (*dl > now) break;  // 还没到 deadline

                if (ringbuffer.available_write() < packet_payload_size) break;

                jitter_buffer.pop_next(std::span<std::byte>{jb_pop_buf.data(), jb_pop_buf.size()});
                ringbuffer.write(std::span<const std::byte>{jb_pop_buf.data(), packet_payload_size});
            }

            schedule_jb_pop();
        });
    };

    // ---- UDP 接收回调 ----
    transport.start_receive([&](const asio::ip::udp::endpoint& /*sender*/,
                                std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (!type) {
            aqua::log_debug_fmt("UDP recv unknown packet type ({} bytes)", data.size());
            return;
        }

        if (*type == aqua::net::PacketType::HelloAck) {
            auto ack = aqua::net::decode_hello(data);
            if (ack && ack->session_id == session_id) {
                bool was_acked = hello_acked.exchange(true, std::memory_order_relaxed);
                if (!was_acked) {
                    aqua::log_info("UDP HELLO_ACK received, channel established");
                }
                recv_hello_acks.fetch_add(1, std::memory_order_relaxed);
                // 后续 HELLO_ACK 是保活响应，静默
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            auto decoded = aqua::net::decode_audio(data);
            if (decoded) {
                // M4: 推入 JitterBuffer 而非直接写 RingBuffer
                jitter_buffer.push(
                    decoded->header.sequence,
                    decoded->header.sample_position,
                    decoded->payload);

                recv_audio_packets.fetch_add(1, std::memory_order_relaxed);
                recv_audio_bytes.fetch_add(decoded->payload.size(), std::memory_order_relaxed);
                // 更新最后收包时间，用于主循环的 server 断开检测
                last_audio_recv_ns.store(
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);
            } else {
                aqua::log_debug_fmt("Failed to decode Audio packet ({} bytes)", data.size());
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

    // ---- 启动 JitterBuffer 调度器 ----
    // 必须在 HELLO_ACK 后立即启动，不等待 WASAPI 初始化。
    // 服务器在 HELLO_ACK 后立即开始广播音频，WASAPI 初始化可能耗时数秒，
    // 如果此时 JitterBuffer 只 push 不 pop，sequence 会快速超过 capacity 导致持续 reset。
    // 调度器会按时 pop 并写入 RingBuffer，WASAPI 准备好后从 RingBuffer 消费。
    asio::post(ioc, [&] { schedule_jb_pop(); });

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

    // 周期性统计日志（每 5 秒输出一次，便于观察接收流量而不刷屏）
    constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
    auto last_stats_time = std::chrono::steady_clock::now();

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // 监控播放后端健康状态：初始化成功后若线程因运行时错误退出
        // （如设备被占用/移除），触发优雅退出，避免客户端空转且持续发送无意义 HELLO 保活。
        if (!playback->is_running()) {
            aqua::log_error("Playback backend stopped unexpectedly, shutting down");
            g_running = false;
        }

        // 检测 server 是否已断开：超过 CLIENT_AUDIO_TIMEOUT 未收到任何 Audio 包，
        // 认为 server 已关闭或网络中断，触发优雅退出。
        // 覆盖场景：server 被 Ctrl+C / kill / 崩溃后，client 不再收到音频数据，
        // 但 playback 仍正常运行（填静音），is_running() 无法感知。
        {
            const auto now = std::chrono::steady_clock::now();
            const auto last_ns = last_audio_recv_ns.load(std::memory_order_relaxed);
            const auto last_time = std::chrono::steady_clock::time_point(
                std::chrono::steady_clock::duration(last_ns));
            if (now - last_time > aqua::config::CLIENT_AUDIO_TIMEOUT) {
                aqua::log_error_fmt("No audio data from server for {}s, server may be down, shutting down",
                                    aqua::config::CLIENT_AUDIO_TIMEOUT.count());
                g_running = false;
            }
        }

        // 周期性输出接收统计
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= STATS_INTERVAL) {
            const auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - last_stats_time).count();
            const auto packets = recv_audio_packets.exchange(0, std::memory_order_relaxed);
            const auto bytes = recv_audio_bytes.exchange(0, std::memory_order_relaxed);
            const auto acks = recv_hello_acks.exchange(0, std::memory_order_relaxed);
            aqua::log_debug_fmt("Client stats: {} audio packets ({:.1f} KB), {} HELLO_ACKs in {:.2f}s ({:.1f} packets/s)",
                                packets,
                                static_cast<double>(bytes) / 1024.0,
                                acks, secs,
                                static_cast<double>(packets) / secs);
            last_stats_time = now;
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
