#include "cli_parser_client.h"
#include "core/audio/backend/audio_backend.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/diagnostics/diagnostics_manager.h"
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
std::atomic<bool> g_running { true };

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

    aqua::set_log_level(parsed.log_level);
    aqua::log_info_fmt("Starting Aqua client, server={}:{}, jitter_latency={}ms",
        parsed.server_ip, parsed.server_rpc_port, parsed.jitter_latency_ms);

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

    asio::ip::udp::endpoint server_udp_endpoint(
        asio::ip::make_address(parsed.server_ip), connect_result.udp_port);

    // Init RingBuffer: JitterBuffer → RingBuffer → 播放线程
    aqua::audio::SpscRingBuffer ringbuffer(aqua::config::PLAYBACK_RINGBUFFER_SIZE);

    // 每包 PCM 参数
    const std::uint32_t frames_per_packet = server_audio_format.sample_rate * aqua::config::AUDIO_PACKET_MS / 1000;
    const std::size_t packet_payload_size = static_cast<std::size_t>(frames_per_packet) * server_audio_format.frame_bytes();

    // M5: 从 CLI 参数 jitter_latency_ms 计算 target_latency_packets
    const std::size_t jitter_target_packets = (parsed.jitter_latency_ms * server_audio_format.sample_rate / 1000) / frames_per_packet;
    // capacity = bit_ceil(target * 2)
    std::size_t jitter_capacity = 8;
    while (jitter_capacity < jitter_target_packets * 2)
        jitter_capacity <<= 1;

    aqua::log_info_fmt("JitterBuffer: target={} packets ({}ms), capacity={} packets",
        jitter_target_packets, parsed.jitter_latency_ms, jitter_capacity);

    // JitterBuffer: packet 时间顺序 + jitter + loss
    aqua::jitter::JitterBuffer jitter_buffer(
        server_audio_format,
        frames_per_packet,
        jitter_target_packets,
        jitter_capacity);

    // UDP 握手状态
    std::atomic<bool> hello_acked { false };

    // 接收统计
    std::atomic<std::uint64_t> recv_audio_packets { 0 };
    std::atomic<std::uint64_t> recv_audio_bytes { 0 };
    std::atomic<std::uint64_t> recv_hello_acks { 0 };

    std::atomic<int64_t> last_audio_recv_ns {
        std::chrono::steady_clock::now().time_since_epoch().count()
    };

    // M5: DiagnosticsManager
    aqua::diag::DiagnosticsManager diag_manager(
        server_audio_format.sample_rate,
        server_audio_format.frame_bytes(),
        packet_payload_size,
        [&ringbuffer]() { return ringbuffer.available_read(); });

    // ---- JitterBuffer → RingBuffer 调度器 ----
    asio::steady_timer jb_timer(ioc);
    std::vector<std::byte> jb_pop_buf(packet_payload_size);

    std::function<void()> schedule_jb_pop;
    schedule_jb_pop = [&]() {
        auto deadline = jitter_buffer.next_playout_deadline();
        if (!deadline) {
            jb_timer.expires_after(std::chrono::milliseconds(10));
        } else {
            // deadline 在过去时（追赶模式），用 1ms 最小间隔防止 busy-loop：
            // RingBuffer 满时 while 循环会 break，但 deadline 仍在过去，
            // expires_at(过去时间) 会让 async_wait 立即返回 → CPU 空转。
            // 1ms 延迟足以让 WASAPI 消费一批数据腾出空间。
            auto now = std::chrono::steady_clock::now();
            if (*deadline <= now) {
                jb_timer.expires_after(std::chrono::milliseconds(1));
            } else {
                jb_timer.expires_at(*deadline);
            }
        }

        jb_timer.async_wait([&](const asio::error_code& ec) {
            if (ec || !g_running) return;

            // 一次性 pop 所有已过 deadline 的包。
            // Windows 默认定时器粒度 ~15.6ms，steady_timer 可能延迟 ~15ms 才触发，
            // 此时多个包的 deadline 已过。若每次只 pop 1 包，RingBuffer 仅获得
            // 3ms 数据，而 WASAPI 每次回调需要 ~10ms → underrun → 破音。
            // 批量 pop 让 RingBuffer 一次性获得多包数据，平滑覆盖到下次 timer 触发。
            const auto now = std::chrono::steady_clock::now();
            const auto max_catchup_ms = std::chrono::milliseconds(
                static_cast<int>(jitter_target_packets * aqua::config::AUDIO_PACKET_MS));

            while (g_running) {
                auto dl = jitter_buffer.next_playout_deadline();
                if (!dl || *dl > now) break;

                auto lateness_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *dl);

                // 如果 deadline 落后超过整个 target_latency，说明发生了长时间断流
                // （如网络中断）。直接 reset 时间线，等下一个包到达时重建。
                if (lateness_ms > max_catchup_ms) {
                    aqua::log_warn_fmt("JitterBuffer: deadline behind by {}ms (>{}, likely stream gap), resetting timeline",
                                       lateness_ms.count(), max_catchup_ms.count());
                    jitter_buffer.reset();
                    break;
                }

                // 调度延迟检测：过期超过 1 个 packet_duration 说明 timer 不及时。
                if (lateness_ms > std::chrono::milliseconds(aqua::config::AUDIO_PACKET_MS)) {
                    diag_manager.on_deadline_miss();
                }

                // RingBuffer 没有空间时停止 pop，保留包在 JitterBuffer 中。
                // 这样当 WASAPI 消费数据腾出空间后，包仍可被 pop（而非被丢弃）。
                // 配合 schedule_jb_pop 的 1ms 最小重调度间隔，避免 busy-loop。
                if (ringbuffer.available_write() < packet_payload_size) {
                    break;
                }

                (void)jitter_buffer.pop_next(std::span<std::byte>{jb_pop_buf.data(), jb_pop_buf.size()});
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
                    // 首个 HELLO_ACK 到达时立即启动 JitterBuffer 调度器。
                    // 不能等待主线程的 HELLO 重试循环 sleep 结束（最长 2 秒），
                    // 否则服务器在此期间持续发送音频，JitterBuffer 只 push 不 pop，
                    // sequence 快速超过 capacity 导致持续 reset。
                    asio::post(ioc, [&] { schedule_jb_pop(); });
                }
                recv_hello_acks.fetch_add(1, std::memory_order_relaxed);
                // M5: RTT 测量
                diag_manager.on_hello_ack_received();
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            auto decoded = aqua::net::decode_audio(data);
            if (decoded) {
                jitter_buffer.push(
                    decoded->header.sequence,
                    decoded->header.sample_position,
                    decoded->payload);

                // M5: 诊断采集
                diag_manager.on_packet_received(decoded->header.sequence,
                    decoded->header.sample_position);

                recv_audio_packets.fetch_add(1, std::memory_order_relaxed);
                recv_audio_bytes.fetch_add(decoded->payload.size(), std::memory_order_relaxed);
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
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf { };
    auto hello_written = aqua::net::encode_hello(session_id, hello_buf);

    while (g_running && !hello_acked.load(std::memory_order_relaxed)) {
        diag_manager.on_hello_sent();
        transport.send(server_udp_endpoint,
            std::span<const std::byte> { hello_buf.data(), hello_written });
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

    // ---- WASAPI Playback ----
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
            auto got = ringbuffer.read(out);
            if (got < out.size()) {
                diag_manager.on_underrun();
            }
            return got;
        })) {
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
    std::thread hello_keepalive_thread([&] {
        while (g_running) {
            std::this_thread::sleep_for(aqua::config::KEEPALIVE_INTERVAL);
            if (!g_running)
                break;

            diag_manager.on_hello_sent();
            transport.send(server_udp_endpoint,
                std::span<const std::byte> { hello_buf.data(), hello_written });
        }
    });

    // 等待退出
    aqua::log_info("Client running. Press Ctrl+C to stop.");

    constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
    auto last_stats_time = std::chrono::steady_clock::now();

    while (g_running) {
        // 50ms 轮询：兼顾响应速度（Ctrl+C 后 <50ms 退出）与 CPU 开销。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!playback->is_running()) {
            aqua::log_error("Playback backend stopped unexpectedly, shutting down");
            g_running = false;
        }

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

        // M5: 周期性诊断日志
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= STATS_INTERVAL) {
            diag_manager.sample_and_log(jitter_buffer, now - last_stats_time);
            last_stats_time = now;
        }
    }

    aqua::log_info("Shutting down...");
    playback->stop();
    g_running = false;
    transport.stop();
    ioc.stop();

    if (ioc_thread.joinable())
        ioc_thread.join();
    if (hello_keepalive_thread.joinable())
        hello_keepalive_thread.join();

    grpc_client.disconnect(session_id);

    aqua::log_info("Client stopped.");
    return 0;
}
