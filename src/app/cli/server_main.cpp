#include "app/cli/cli_parser_server.h"
#include "core/audio/backend/audio_backend_factory.h"
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
#include <semaphore>
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

    aqua::set_log_level(parsed.log_level);
    aqua::log_info_fmt("Starting Aqua server on {} gRPC={}, UDP={}",
                       parsed.bind_ip, parsed.rpc_port, parsed.udp_port);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---- 运行时配置 ----
    aqua::config::RuntimeConfig rt_cfg;
    if (parsed.capture_buffer_size > 0)
        rt_cfg.capture_ringbuffer_size = parsed.capture_buffer_size;

    // ---- WASAPI Loopback Capture（先启动，获取 AudioFormat 给 gRPC）----
    // 启动顺序：WASAPI -> gRPC(控制面) -> UDP(数据面) -> 其余线程
    // 失败路径：任何步骤失败时，之前已启动的资源按逆序清理。
    auto capture = aqua::audio::create_capture_backend();
    if (!capture) {
        std::cerr << "Error: no audio capture backend available\n";
        return 1;
    }

    aqua::audio::SpscRingBuffer ringbuffer(rt_cfg.capture_ringbuffer_size);
    aqua::log_info_fmt("Capture RingBuffer: requested={} bytes, actual={} bytes",
                       rt_cfg.capture_ringbuffer_size, ringbuffer.capacity());

    // 跟踪 RingBuffer 溢出丢字节数（capture 写入但 RingBuffer 放不下的部分）。
    // 用 atomic 因为 capture 回调在音频线程，packetizer 统计在 sender 线程读取。
    std::atomic<std::uint64_t> capture_dropped_bytes{0};

    // capture → packetizer 数据就绪通知。
    // binary_semaphore: capture 回调 release() 后，packetizer 的 try_acquire_for
    // 立即返回（OS 事件机制，不受 Windows 15.6ms 定时器粒度影响）。
    // 替代 yield()（busy-loop 12% CPU）和 sleep_for（oversleep 导致 262pps 丢数据）。
    std::binary_semaphore capture_sem{0};

    aqua::AudioFormat capture_format{};

    if (!capture->start([&](std::span<const std::byte> pcm) {
            auto written = ringbuffer.write(pcm);
            if (written < pcm.size()) {
                capture_dropped_bytes.fetch_add(pcm.size() - written,
                                                std::memory_order_relaxed);
            }
            capture_sem.release(); // 立即唤醒 packetizer 线程
        }, capture_format)) {
        std::cerr << "Error: failed to start audio capture\n";
        return 1;
    }

    aqua::log_info_fmt("Capture format: {}ch {}Hz encoding={}",
                       capture_format.channels, capture_format.sample_rate,
                       static_cast<int>(capture_format.encoding));

    // ---- SessionManager ----
    aqua::SessionManager sessions;

    // ---- gRPC Server（控制面先就绪，client 可先 Connect 拿到 session_id）----
    aqua::grpc::GrpcServer grpc_server(
        sessions, capture_format,
        parsed.bind_ip, parsed.rpc_port,
        parsed.bind_ip, parsed.udp_port);

    std::thread grpc_thread([&] {
        grpc_server.run();
    });

    // 检测 gRPC 是否成功启动；失败则等待 grpc_thread 退出后清理下层资源。
    // 给 BuildAndStart 一点时间完成（它在构造函数里同步完成，但 is_running 标志在 run() 里置位）。
    {
        bool grpc_ok = false;
        for (int i = 0; i < 50; ++i) {
            if (grpc_server.is_running()) { grpc_ok = true; break; }
            if (!g_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!grpc_ok) {
            std::cerr << "Error: failed to start gRPC server on " << parsed.rpc_port << "\n";
            g_running = false;
            grpc_server.shutdown();
            if (grpc_thread.joinable()) grpc_thread.join();
            capture->stop();
            return 1;
        }
    }

    // ---- UDP Transport（数据面）----
    // UDP 绑定失败时需先关闭已启动的 gRPC server 再退出。
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind(parsed.bind_ip, parsed.udp_port)) {
        std::cerr << "Error: failed to bind UDP port " << parsed.udp_port << "\n";
        g_running = false;
        grpc_server.shutdown();
        if (grpc_thread.joinable()) grpc_thread.join();
        capture->stop();
        return 1;
    }
    aqua::log_info_fmt("UDP bound to {}:{}", parsed.bind_ip, parsed.udp_port);

    // UDP 接收回调：处理 HELLO / AUDIO
    transport.start_receive([&](const asio::ip::udp::endpoint& sender,
                                std::span<const std::byte> data) {
        auto type = aqua::net::peek_type(data);
        if (!type) {
            aqua::log_debug_fmt("UDP recv unknown packet type from {}:{} ({} bytes)",
                                sender.address().to_string(), sender.port(), data.size());
            return;
        }

        if (*type == aqua::net::PacketType::Hello) {
            auto hello = aqua::net::decode_hello(data);
            if (hello) {
                // HELLO 兼任两种角色：
                //   1. 首次握手（Created -> Connected）
                //   2. UDP keepalive（已 Connected，刷新 NAT 映射 + last_seen）
                bool was_connected = sessions.is_connected(hello->session_id);
                if (sessions.establish_udp(hello->session_id, sender)) {
                    if (!was_connected) {
                        // 首次握手
                        aqua::log_info_fmt("Session 0x{:08X} UDP established: {}:{}",
                                           hello->session_id,
                                           sender.address().to_string(), sender.port());
                    } else {
                        // keepalive: endpoint 可能因 NAT remap 变化，记录 debug
                        aqua::log_trace_fmt("Session 0x{:08X} HELLO keepalive from {}:{}",
                                            hello->session_id,
                                            sender.address().to_string(), sender.port());
                    }
                    // 始终回复 HELLO_ACK（首次握手需要，keepalive 也可用于确认链路）
                    std::array<std::byte, sizeof(aqua::net::HelloPacket)> ack_buf{};
                    aqua::net::encode_hello_ack(hello->session_id, ack_buf);
                    transport.send(sender,
                                   std::span<const std::byte>{ack_buf.data(), ack_buf.size()});
                } else {
                    aqua::log_warn_fmt("HELLO from unknown session 0x{:08X} (from {}:{})",
                                       hello->session_id,
                                       sender.address().to_string(), sender.port());
                }
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            // 当前为单向音频（server -> client），server 不应收到 Audio 包。
            // 若收到（恶意/bug client），直接丢弃，不 touch_session —— 否则
            // client 持续发 Audio 包会让它的 session 永不过期。
            aqua::log_debug_fmt("Server received unexpected Audio packet from {}:{} ({} bytes), dropping",
                                sender.address().to_string(), sender.port(), data.size());
        }
    });

    std::thread ioc_thread([&] {
        ioc.run();
    });

    // ---- Session 超时清理定时器（替代独立线程）----
    // 挂在 io_context 上，SessionManager 内部有 mutex_ 保证线程安全。
    asio::steady_timer cleanup_timer(ioc);
    std::function<void()> schedule_cleanup;
    schedule_cleanup = [&]() {
        cleanup_timer.expires_after(aqua::config::EXPIRED_CLEANUP_INTERVAL);
        cleanup_timer.async_wait([&](const asio::error_code& ec) {
            if (ec || !g_running) return;
            auto expired = sessions.collect_expired_sessions(aqua::config::UDP_SESSION_TIMEOUT);
            for (auto id : expired) {
                aqua::log_info_fmt("Session 0x{:08X} expired, removing", id);
                sessions.remove_session(id);
            }
            schedule_cleanup();
        });
    };
    schedule_cleanup();

    // ---- Packetizer Thread ----
    std::thread sender_thread([&] {
        // FRAMES_PER_PACKET 是固定帧数（与采样率无关），packet_duration 由它推导，
        // 任何采样率下都精确等于音频内容真实时长，无截断漂移。
        const std::uint32_t frames_per_packet = aqua::config::AUDIO_FRAMES_PER_PACKET;
        const std::size_t packet_payload_size =
            frames_per_packet * capture_format.frame_bytes();
        const std::size_t send_buf_size = sizeof(aqua::net::AudioPacketHeader) + packet_payload_size;

        aqua::log_info_fmt("Packetizer: {} frames/packet, payload={}B, wire={}B",
                           frames_per_packet, packet_payload_size, send_buf_size);

        std::vector<std::byte> send_buf(send_buf_size);
        std::vector<std::byte> pcm_buf(packet_payload_size);

        std::uint32_t sequence = 0;
        std::uint64_t sample_position = 0;

        // 周期性统计日志（每 5 秒输出一次，便于观察发送流量而不刷屏）
        constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
        auto last_stats_time = std::chrono::steady_clock::now();
        std::uint64_t stats_packets = 0;
        std::uint64_t stats_bytes = 0;
        std::uint64_t stats_pcm_bytes = 0; // 纯 PCM 负载字节数（不含包头）

        while (g_running) {
            // 从 RingBuffer 读取一包数据。
            // 3ms packet + 10ms WASAPI capture 天然产生跨 callback 的残余数据
            // （3840 / 1152 = 3 余 384 bytes），残余数据必须跨 WASAPI callback 保留，
            // 不能因为 RingBuffer 短暂为空就丢弃。
            std::size_t got = 0;

            while (got < pcm_buf.size() && g_running) {
                got += ringbuffer.read(std::span<std::byte>{
                    pcm_buf.data() + got, pcm_buf.size() - got});
                if (got < pcm_buf.size()) {
                    // 数据不足：阻塞等待 capture 回调通知。
                    // release() 通过 OS 事件立即唤醒本线程（不受 Windows 定时器粒度影响）。
                    // 100ms 超时仅用于定期检查 g_running（如 Ctrl+C 停止）。
                    // 线程在等待期间不消耗 CPU（非 busy-loop）。
                    // try_acquire_for() 返回 bool 表示是否成功获取信号量,这里我们不关心结果(只是等待唤醒),用 (void) 忽略即可。
                    (void)capture_sem.try_acquire_for(std::chrono::milliseconds(100));
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
                std::size_t recipients = 0;
                sessions.for_each_connected([&](auto /*id*/, const auto& endpoint) {
                    transport.send(endpoint,
                                   std::span<const std::byte>{send_buf.data(), written});
                    ++recipients;
                    return true; // 继续遍历
                });
                ++stats_packets;
                stats_bytes += written;
                stats_pcm_bytes += got;
                sequence++;
                sample_position += frames_per_packet;
            }

            // 周期性输出发送统计
            const auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= STATS_INTERVAL) {
                const auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - last_stats_time).count();
                const auto session_count = sessions.session_count();
                const auto dropped = capture_dropped_bytes.exchange(0, std::memory_order_relaxed);
                aqua::log_debug_fmt("Packetizer stats: {} packets, {:.1f} KB in {:.2f}s ({:.1f} packets/s), {} active session(s), pcm={:.1f} KB, dropped={:.1f} KB",
                                    stats_packets,
                                    static_cast<double>(stats_bytes) / 1024.0,
                                    secs,
                                    static_cast<double>(stats_packets) / secs,
                                    session_count,
                                    static_cast<double>(stats_pcm_bytes) / 1024.0,
                                    static_cast<double>(dropped) / 1024.0);
                stats_packets = 0;
                stats_bytes = 0;
                stats_pcm_bytes = 0;
                last_stats_time = now;
            }
        }
    });

    // 等待退出
    aqua::log_info("Server running. Press Ctrl+C to stop.");
    while (g_running) {
        // 50ms 轮询：兼顾响应速度（Ctrl+C 后 <50ms 退出）与 CPU 开销。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // 监控采集后端健康状态：初始化成功后若线程因运行时错误退出
        // （如设备被禁用/移除），触发优雅退出，避免 server 继续向 client 发送空数据。
        if (!capture->is_running()) {
            aqua::log_error("Capture backend stopped unexpectedly, shutting down");
            g_running = false;
        }
        // 监控 gRPC server 健康状态：若 gRPC 线程异常退出（端口被占用后 Wait 立即返回等），
        // 触发优雅退出，避免 server 在没有控制面的情况下继续运行。
        if (!grpc_server.is_running()) {
            aqua::log_error("gRPC server stopped unexpectedly, shutting down");
            g_running = false;
        }
    }

    aqua::log_info("Shutting down...");
    capture->stop();
    g_running = false;
    grpc_server.shutdown();
    transport.stop();
    ioc.stop();

    if (grpc_thread.joinable()) grpc_thread.join();
    if (ioc_thread.joinable()) ioc_thread.join();
    if (sender_thread.joinable()) sender_thread.join();

    // 清理残留 session（如 client 仍在线但 server 被强制关闭的情况），
    // 避免 SessionManager 析构时的 warning 日志。
    sessions.clear();

    aqua::log_info("Server stopped.");
    return 0;
}
