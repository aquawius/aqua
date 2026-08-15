#include "app/cli/cli_parser_client.h"
#include "core/audio/backend/audio_backend_factory.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/diagnostics/diagnostics_manager.h"
#include "core/grpc/grpc_client.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/config.h"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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

// 单次会话的结果，供 main 决定是否重连。
enum class SessionOutcome {
    CleanExit, // 用户 Ctrl+C（g_running 被置 false）
    Retryable, // 可重连：gRPC 失败 / HELLO 超时 / 音频超时
    Fatal,     // 不可恢复：格式非法 / UDP 绑定失败 / 无播放后端 / 播放失败
};

// 执行一次完整的客户端会话：gRPC Connect → UDP 握手 → 播放 → 主循环 → 清理。
// 返回结果表示"为何退出"，由 main 决定是否指数退避重连。
SessionOutcome run_session(const aqua::ClientCliResult& parsed);

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

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!parsed.auto_reconnect) {
        auto outcome = run_session(parsed);
        return (outcome == SessionOutcome::Fatal) ? 1 : 0;
    }

    // ---- 自动重连（指数退避）----
    int attempt = 0;
    auto session_start = std::chrono::steady_clock::now();
    while (g_running) {
        auto outcome = run_session(parsed);
        if (outcome == SessionOutcome::CleanExit) {
            break;
        }
        if (outcome == SessionOutcome::Fatal) {
            return 1;
        }

        // Retryable：指数退避后重连。
        // 上次会话稳定运行过（>= RECONNECT_BACKOFF_RESET_AFTER）则重置退避，避免断线后仍等 30s。
        const auto session_duration = std::chrono::steady_clock::now() - session_start;
        if (session_duration >= aqua::config::RECONNECT_BACKOFF_RESET_AFTER) {
            attempt = 0;
        }

        const int exp = std::min(attempt, 5); // 2^5 = 32s，再往上封顶
        auto delay = std::chrono::seconds(1 << exp);
        if (delay > aqua::config::RECONNECT_MAX_DELAY) {
            delay = aqua::config::RECONNECT_MAX_DELAY;
        }
        aqua::log_info_fmt("Reconnecting in {}s (attempt {})", delay.count(), attempt + 1);
        ++attempt;

        // 分段 sleep，便于 Ctrl+C 及时中断退避等待。
        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (g_running && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        session_start = std::chrono::steady_clock::now();
    }

    aqua::log_info("Client stopped.");
    return 0;
}

namespace {

SessionOutcome run_session(const aqua::ClientCliResult& parsed)
{
    // ---- gRPC Connect ----
    aqua::grpc::GrpcClient grpc_client;
    if (!grpc_client.connect_to_server(parsed.server_ip, parsed.server_rpc_port)) {
        std::cerr << "Error: failed to connect to gRPC server\n";
        return SessionOutcome::Retryable;
    }

    aqua::grpc::ConnectResult connect_result;
    if (!grpc_client.connect("aqua_client", connect_result)) {
        std::cerr << "Error: gRPC Connect failed\n";
        return SessionOutcome::Retryable;
    }

    const auto session_id = connect_result.session_id;
    const auto& server_audio_format = connect_result.audio_format;

    if (!server_audio_format.valid()) {
        std::cerr << "Error: server returned invalid audio format\n";
        grpc_client.disconnect(session_id);
        return SessionOutcome::Fatal;
    }

    aqua::log_info_fmt("Server audio format: {}ch {}Hz encoding={}",
        server_audio_format.channels, server_audio_format.sample_rate,
        static_cast<int>(server_audio_format.encoding));

    // ---- 运行时配置 ----
    aqua::config::RuntimeConfig rt_cfg;
    if (parsed.jitter_latency_ms > 0)
        rt_cfg.jitter_target_latency_ms = parsed.jitter_latency_ms;
    if (parsed.drift_late_threshold > 0)
        rt_cfg.jitter_drift_late_threshold = parsed.drift_late_threshold;
    if (parsed.playback_buffer_size > 0)
        rt_cfg.playback_ringbuffer_size = parsed.playback_buffer_size;

    aqua::log_info_fmt("Starting Aqua client, server={}:{}, jitter_latency={}ms",
        parsed.server_ip, parsed.server_rpc_port, rt_cfg.jitter_target_latency_ms);

    // ---- UDP Transport ----
    asio::io_context ioc;
    aqua::net::UdpTransport transport(ioc);
    if (!transport.bind("0.0.0.0", 0)) {
        std::cerr << "Error: failed to bind local UDP port\n";
        grpc_client.disconnect(session_id);
        return SessionOutcome::Fatal;
    }

    auto local_ep = transport.socket_local_endpoint();
    aqua::log_info_fmt("Client UDP bound to {}:{}", local_ep.address().to_string(), local_ep.port());

    // asio::ip::make_address 在 IP 格式非法时抛异常，需 try-catch 保护。
    asio::ip::address server_address;
    try {
        server_address = asio::ip::make_address(parsed.server_ip);
    } catch (const std::exception& e) {
        std::cerr << "Error: invalid server IP address '" << parsed.server_ip
                  << "': " << e.what() << "\n";
        grpc_client.disconnect(session_id);
        return SessionOutcome::Fatal;
    }
    asio::ip::udp::endpoint server_udp_endpoint(server_address, connect_result.udp_port);

    // Init RingBuffer: JitterBuffer → RingBuffer → 播放线程
    // SpscRingBuffer 容量向上取整为 1KiB 的倍数，构造时自动对齐。
    aqua::audio::SpscRingBuffer ringbuffer(rt_cfg.playback_ringbuffer_size);
    // 字节速率（B/ms），把容量换算成时长，便于直观比较缓冲余量。
    const double bytes_per_ms = static_cast<double>(server_audio_format.sample_rate)
                              * server_audio_format.frame_bytes() / 1000.0;
    aqua::log_info_fmt("Playback RingBuffer: requested={} bytes ({:.1f}ms), actual={} bytes ({:.1f}ms)",
        rt_cfg.playback_ringbuffer_size, rt_cfg.playback_ringbuffer_size / bytes_per_ms,
        ringbuffer.capacity(), ringbuffer.capacity() / bytes_per_ms);

    // 每包 PCM 参数。FRAMES_PER_PACKET 是固定帧数（与采样率无关），
    // 由它推导的 packet_duration = frames/sample_rate 精确等于音频内容时长，无截断漂移。
    const std::uint32_t frames_per_packet = aqua::config::AUDIO_FRAMES_PER_PACKET;
    const std::size_t packet_payload_size = static_cast<std::size_t>(frames_per_packet) * server_audio_format.frame_bytes();
    // 实际每包时长（微秒）。
    // 注意：JitterBuffer 内部的 packet_duration_ 用纳秒（见 jitter_buffer.h），
    // 以避免 44.1kHz 家族在微秒整数除法下被截断、逐包累积成漂移。
    // 这里保留微秒是刻意的——packet_duration_us 只用于 max_catchup（30ms 级）
    // 和 deadline-miss（3ms 级）两处阈值比较，不参与任何逐包累加，
    // 0.3μs 的截断误差对这类阈值完全无感，无需纳秒精度。
    const auto packet_duration_us = std::chrono::microseconds(
        static_cast<std::int64_t>(frames_per_packet) * 1'000'000 / server_audio_format.sample_rate);

    const double packet_duration_ms = static_cast<double>(packet_duration_us.count()) / 1000.0;
    const std::size_t packet_wire_size = sizeof(aqua::net::AudioPacketHeader) + packet_payload_size;
    aqua::log_info_fmt("Audio packet: {} frames/packet ({:.2f}ms @ {}Hz), payload={}B, wire={}B",
                       frames_per_packet, packet_duration_ms,
                       server_audio_format.sample_rate, packet_payload_size, packet_wire_size);

    // 从 rt_cfg 的目标延迟（ms）计算 target_latency_packets
    std::size_t jitter_target_packets = (rt_cfg.jitter_target_latency_ms * server_audio_format.sample_rate / 1000) / frames_per_packet;
    // 整除截断保护：jitter-latency 小于 1 个包时长时整除结果为 0，
    // 会导致零缓冲。强制下限为 1 包并提示用户最小有效延迟。
    if (jitter_target_packets == 0 && rt_cfg.jitter_target_latency_ms > 0) {
        jitter_target_packets = 1;
        aqua::log_warn_fmt("jitter-latency {}ms below 1 packet ({:.2f}ms), clamped to 1 packet",
            rt_cfg.jitter_target_latency_ms, packet_duration_ms);
    }
    // capacity = bit_ceil(target * 2)
    std::size_t jitter_capacity = 8;
    while (jitter_capacity < jitter_target_packets * 2)
        jitter_capacity <<= 1;

    aqua::log_info_fmt("JitterBuffer: target={} packets (req {}ms, actual {:.2f}ms, {}B), capacity={} packets ({:.2f}ms, {}B)",
        jitter_target_packets, rt_cfg.jitter_target_latency_ms, packet_duration_ms * jitter_target_packets, jitter_target_packets * packet_payload_size,
        jitter_capacity, packet_duration_ms * jitter_capacity, jitter_capacity * packet_payload_size);

    // JitterBuffer: packet 时间顺序 + jitter + loss
    aqua::jitter::JitterBuffer jitter_buffer(
        server_audio_format,
        frames_per_packet,
        jitter_target_packets,
        jitter_capacity,
        rt_cfg.jitter_drift_window_size,
        rt_cfg.jitter_drift_late_threshold);

    // UDP 握手状态
    std::atomic<bool> hello_acked { false };

    // WASAPI playback 初始化标志：在 playback 启动前丢弃音频包，
    // 避免 JB 在无消费者时溢出导致启动期 sequence jump rebase 刷屏。
    std::atomic<bool> playback_ready { false };

    std::atomic<int64_t> last_audio_recv_ns {
        std::chrono::steady_clock::now().time_since_epoch().count()
    };

    // 已播放样本累计（播放线程累加，主线程读，relaxed）
    std::atomic<std::uint64_t> played_samples{0};

    // keepalive HELLO 连续未收到 ACK 计数 + 告警去重标志。
    // 两者都只在 io_context 单线程（recv 回调 + keepalive 定时器）访问，无需 atomic。
    std::uint32_t consecutive_missed_acks = 0;
    bool keepalive_loss_warned = false;

    // M5: DiagnosticsManager
    aqua::diag::DiagnosticsManager diag_manager(
        server_audio_format.sample_rate,
        server_audio_format.frame_bytes(),
        packet_payload_size,
        [&ringbuffer]() { return ringbuffer.available_read(); },
        ringbuffer.capacity(),
        [&played_samples]() { return played_samples.load(std::memory_order_relaxed); });

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
            // Windows 默认定时器粒度 ~15ms，steady_timer 可能延迟 ~15ms 才触发，
            // 此时多个包的 deadline 已过。若每次只 pop 1 包，RingBuffer 仅获得
            // 3ms 数据，而 WASAPI 每次回调需要 ~10ms → underrun → 破音。
            // 批量 pop 让 RingBuffer 一次性获得多包数据，平滑覆盖到下次 timer 触发。
            const auto now = std::chrono::steady_clock::now();
            // catchup 上限 = 整个 target_latency（与采样率无关，用实际 packet_duration）
            const auto max_catchup = packet_duration_us * jitter_target_packets;

            while (g_running) {
                auto dl = jitter_buffer.next_playout_deadline();
                if (!dl || *dl > now) break;

                // RingBuffer 没有空间时停止 pop，保留包在 JitterBuffer 中。
                // 必须在 max_catchup 检查之前 break：WASAPI 未启动时 RB 满，
                // deadline 会持续积累 lateness，但这是"无法消费"而非"断流"，
                // 不应触发 reset 警告。WASAPI 启动后 RB 有空间，才会检查 lateness。
                if (ringbuffer.available_write() < packet_payload_size) {
                    break;
                }

                // lateness 用微秒精度，避免与 packet_duration_us (微秒) 比较时丢精度。
                auto lateness = std::chrono::duration_cast<std::chrono::microseconds>(now - *dl);

                // 如果 deadline 落后超过整个 target_latency，说明发生了长时间断流
                // （如网络中断）。直接 reset 时间线，等下一个包到达时重建。
                if (lateness > max_catchup) {
                    aqua::log_warn_fmt("JitterBuffer: deadline behind by {}us (>{}, likely stream gap), resetting timeline",
                                       lateness.count(), max_catchup.count());
                    jitter_buffer.reset();
                    break;
                }

                // 调度延迟检测：过期超过 1 个 packet_duration 说明 timer 不及时。
                if (lateness > packet_duration_us) {
                    diag_manager.record_deadline_miss();
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
                // 收到 ACK：重置保活丢 ACK 计数（io_context 线程独占，无并发）。
                consecutive_missed_acks = 0;
                keepalive_loss_warned = false;

                bool was_acked = hello_acked.exchange(true, std::memory_order_relaxed);
                if (!was_acked) {
                    aqua::log_info("UDP HELLO_ACK received, channel established");
                    // 首个 HELLO_ACK 到达时立即启动 JitterBuffer 调度器。
                    // 不能等待主线程的 HELLO 重试循环 sleep 结束（最长 2 秒），
                    // 否则服务器在此期间持续发送音频，JitterBuffer 只 push 不 pop，
                    // sequence 快速超过 capacity 导致持续 reset。
                    asio::post(ioc, [&] { schedule_jb_pop(); });
                }
                // M5: RTT 测量 + HELLO_ACK 计数
                diag_manager.record_hello_ack_received();
                diag_manager.record_hello_ack();
            }
        } else if (*type == aqua::net::PacketType::Audio) {
            auto decoded = aqua::net::decode_audio(data);
            if (decoded) {
                // WASAPI playback 未就绪时丢弃音频包，不 push 到 JB。
                // 否则 JB 在无消费者（RB 满→无法 pop）时快速溢出 capacity，
                // 每 32 包触发一次 sequence jump rebase，刷屏 warning。
                if (!playback_ready.load(std::memory_order_relaxed)) {
                    return;
                }

                jitter_buffer.push(
                    decoded->header.sequence,
                    decoded->header.sample_position,
                    decoded->payload);

                // M5: 诊断采集
                diag_manager.record_packet_arrival(decoded->header.sequence,
                    decoded->header.sample_position);
                diag_manager.record_audio_bytes(decoded->payload.size());

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

    // ---- 发送 HELLO 直到收到 HELLO_ACK 或超时 ----
    std::array<std::byte, sizeof(aqua::net::HelloPacket)> hello_buf { };
    auto hello_written = aqua::net::encode_hello(session_id, hello_buf);

    // HELLO 握手超时：HELLO_HANDSHAKE_RETRY_INTERVAL × HELLO_HANDSHAKE_MAX_ATTEMPTS（见 config.h）。
    int hello_attempts = 0;
    while (g_running && !hello_acked.load(std::memory_order_relaxed)) {
        if (++hello_attempts > aqua::config::HELLO_HANDSHAKE_MAX_ATTEMPTS) {
            break;
        }
        aqua::log_debug_fmt("Sending HELLO attempt {}/{} to {}",
                           hello_attempts, aqua::config::HELLO_HANDSHAKE_MAX_ATTEMPTS,
                           parsed.server_ip);
        diag_manager.record_hello_sent();
        transport.send(server_udp_endpoint,
            std::span<const std::byte> { hello_buf.data(), hello_written });
        std::this_thread::sleep_for(aqua::config::HELLO_HANDSHAKE_RETRY_INTERVAL);
    }

    if (!hello_acked.load(std::memory_order_relaxed)) {
        std::cerr << "Error: UDP HELLO_ACK timeout (" << hello_attempts
                  << " attempts, " << (hello_attempts * aqua::config::HELLO_HANDSHAKE_RETRY_INTERVAL.count()) << "ms)\n";
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return SessionOutcome::Retryable;
    }

    // ---- WASAPI Playback ----
    auto playback = aqua::audio::create_playback_backend();
    if (!playback) {
        std::cerr << "Error: no audio playback backend available\n";
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return SessionOutcome::Fatal;
    }

    if (!playback->start(server_audio_format, [&](std::span<std::byte> out) -> std::size_t {
            auto got = ringbuffer.read(out);
            if (got < out.size()) {
                diag_manager.record_underrun();
            }
            // 整个 out 缓冲都会被播放（含静音填充），累加已播放样本数
            played_samples.fetch_add(out.size() / server_audio_format.frame_bytes(),
                                     std::memory_order_relaxed);
            return got;
        })) {
        std::cerr << "Error: failed to start audio playback (see log above for details)\n";
        transport.stop();
        ioc.stop();
        ioc_thread.join();
        grpc_client.disconnect(session_id);
        return SessionOutcome::Fatal;
    }

    aqua::log_info("Playback started with server audio format");
    playback_ready.store(true, std::memory_order_relaxed);
    // 重置音频超时计时器：HELLO 握手 + playback 初始化可能消耗大部分 CLIENT_AUDIO_RECV_TIMEOUT，
    // 从 playback 就绪时刻重新计时，避免误触发 "server may be down" 退出。
    last_audio_recv_ns.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);

    // ---- UDP HELLO 保活定时器（替代独立线程）----
    // 挂在 io_context 上，与 UDP recv 串行执行，无并发问题。
    asio::steady_timer keepalive_timer(ioc);
    std::function<void()> schedule_keepalive;
    schedule_keepalive = [&]() {
        keepalive_timer.expires_after(aqua::config::HELLO_KEEPALIVE_INTERVAL);
        keepalive_timer.async_wait([&](const asio::error_code& ec) {
            if (ec || !g_running) return;
            aqua::log_trace_fmt("HELLO keepalive sent to {}:{} (session=0x{:08X})",
                                parsed.server_ip, connect_result.udp_port, session_id);
            // 连续未收到 ACK 计数：早于音频超时暴露服务器已断。
            ++consecutive_missed_acks;
            if (!keepalive_loss_warned
                && consecutive_missed_acks >= aqua::config::HELLO_ACK_WARN_THRESHOLD) {
                keepalive_loss_warned = true;
                aqua::log_warn_fmt("No HELLO_ACK for {} consecutive keepalives ({}s), server may be down",
                    consecutive_missed_acks,
                    consecutive_missed_acks * aqua::config::HELLO_KEEPALIVE_INTERVAL.count());
            }
            diag_manager.record_hello_sent();
            transport.send(server_udp_endpoint,
                std::span<const std::byte>{hello_buf.data(), hello_written});
            schedule_keepalive();
        });
    };
    schedule_keepalive();

    // 等待退出
    aqua::log_info("Client running. Press Ctrl+C to stop.");

    constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
    constexpr auto RB_SAMPLE_INTERVAL = std::chrono::milliseconds(500);
    auto last_stats_time = std::chrono::steady_clock::now();
    auto last_rb_sample_time = last_stats_time;

    SessionOutcome outcome = SessionOutcome::CleanExit;
    while (g_running) {
        // 50ms 轮询：兼顾响应速度（Ctrl+C 后 <50ms 退出）与 CPU 开销。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!playback->is_running()) {
            aqua::log_error("Playback backend stopped unexpectedly, shutting down");
            outcome = SessionOutcome::Fatal;
            break;
        }

        {
            const auto now = std::chrono::steady_clock::now();
            const auto last_ns = last_audio_recv_ns.load(std::memory_order_relaxed);
            const auto last_time = std::chrono::steady_clock::time_point(
                std::chrono::steady_clock::duration(last_ns));
            if (now - last_time > aqua::config::CLIENT_AUDIO_RECV_TIMEOUT) {
                aqua::log_error_fmt("No audio data from server for {}s, server may be down",
                    aqua::config::CLIENT_AUDIO_RECV_TIMEOUT.count());
                outcome = SessionOutcome::Retryable;
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();

        // 高频采样 RB 占用到 slope 窗口（与日志输出解耦）
        if (now - last_rb_sample_time >= RB_SAMPLE_INTERVAL) {
            diag_manager.record_rb_occupancy();
            last_rb_sample_time = now;
        }

        // 周期性诊断日志
        if (now - last_stats_time >= STATS_INTERVAL) {
            diag_manager.collect_and_log(jitter_buffer);
            last_stats_time = now;
        }
    }

    aqua::log_info("Shutting down...");
    playback->stop();

    // 先通知 server 移除 session 并停止发包，再关闭本地 UDP。
    // 否则 server 在收到 disconnect 前仍向已关闭的 client 端点发包，
    // 触发 ICMP port unreachable 风暴（333 pps × ~1s = 数百条错误日志）。
    grpc_client.disconnect(session_id);

    transport.stop();
    ioc.stop();

    if (ioc_thread.joinable())
        ioc_thread.join();

    return outcome;
}

} // namespace
