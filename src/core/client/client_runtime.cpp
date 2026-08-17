#include "core/client/client_runtime.h"

#include "core/audio/backend/audio_backend_factory.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/diagnostics/diagnostics_manager.h"
#include "core/grpc/grpc_client.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace aqua::client {

namespace {

// 单次会话的结果，供 session_loop 决定是否退避重连。
enum class SessionOutcome {
    CleanExit, // 收到关闭请求（shutdown_requested_ 被置位）
    Retryable, // 可重连：gRPC 失败 / HELLO 超时 / 音频超时
    Fatal,     // 不可恢复：格式非法 / UDP 绑定失败 / 无播放后端 / 播放失败
};

// 主循环 / 退避等待轮询间隔。
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);

// RB 占用高频采样间隔（slope 窗口输入，与诊断刷新解耦——5s 窗口只有 1-2 个
// 样本点时线性回归无意义）。
constexpr auto RB_SAMPLE_INTERVAL = std::chrono::milliseconds(500);

} // namespace

struct ClientRuntime::Impl {
    ClientConfig cfg;
    ClientCallbacks cb;

    std::atomic<bool> shutdown_requested_ { false };
    std::atomic<bool> running_ { false };
    std::atomic<ClientState> state_ { ClientState::Idle };

    mutable std::mutex error_mutex_;
    std::string last_error_;

    // 诊断快照缓存：run_one_session 的 collect_and_log（会话线程）写入，
    // diagnostics()（任意线程）读取，用 mutex 保护。
    mutable std::mutex diag_mutex_;
    std::optional<diag::DiagnosticsManager::Snapshot> last_diag_snapshot_;

    // 服务器音频格式缓存：gRPC Connect 成功（会话线程）写入，
    // audio_format()（任意线程）读取，与诊断快照共用一把锁（低频访问）。
    std::optional<AudioFormat> audio_format_;

    std::thread session_thread;

    void set_last_error(std::string message)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = std::move(message);
    }

    void set_state(ClientState next)
    {
        state_.store(next, std::memory_order_relaxed);
        if (cb.on_state_change) {
            cb.on_state_change(next);
        }
    }

    // 执行一次完整的客户端会话：gRPC Connect → UDP 握手 → 播放 → 主循环 → 清理。
    // 返回结果表示"为何退出"，由 session_loop 决定是否指数退避重连。
    SessionOutcome run_one_session()
    {
        // 新会话开始：清空上一会话的诊断快照（diag_manager 每会话重建，计数器归零，
        // 旧快照若不清除会在重连间隙被 diagnostics() 误读为当前会话数据）。
        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            last_diag_snapshot_.reset();
            audio_format_.reset();
        }

        // ---- gRPC Connect ----
        grpc::GrpcClient grpc_client;
        if (!grpc_client.connect_to_server(cfg.server_ip, cfg.server_rpc_port)) {
            set_last_error("failed to connect to gRPC server at " + cfg.server_ip + ":"
                           + std::to_string(cfg.server_rpc_port));
            log_error("failed to connect to gRPC server");
            return SessionOutcome::Retryable;
        }

        grpc::ConnectResult connect_result;
        if (!grpc_client.connect(cfg.client_name, connect_result)) {
            set_last_error("gRPC Connect failed (server may not be running)");
            log_error("gRPC Connect failed");
            return SessionOutcome::Retryable;
        }

        const auto session_id = connect_result.session_id;
        const auto& server_audio_format = connect_result.audio_format;

        if (!server_audio_format.valid()) {
            set_last_error("server returned invalid audio format");
            log_error("server returned invalid audio format");
            grpc_client.disconnect(session_id);
            return SessionOutcome::Fatal;
        }

        log_info_fmt("Server audio format: {}ch {}Hz encoding={}",
                     server_audio_format.channels, server_audio_format.sample_rate,
                     static_cast<int>(server_audio_format.encoding));
        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            audio_format_ = server_audio_format;
        }
        if (cb.on_format) {
            cb.on_format(server_audio_format);
        }

        const auto& rt_cfg = cfg.runtime;
        log_info_fmt("Starting Aqua client, server={}:{}, jitter_buffer={}ms",
                     cfg.server_ip, cfg.server_rpc_port,
                     rt_cfg.jitter_buffer_ms);

        // ---- UDP Transport ----
        asio::io_context ioc;
        net::UdpTransport transport(ioc);
        if (!transport.bind("0.0.0.0", 0)) {
            set_last_error("failed to bind local UDP port");
            log_error("failed to bind local UDP port");
            grpc_client.disconnect(session_id);
            return SessionOutcome::Fatal;
        }

        const auto local_ep = transport.socket_local_endpoint();
        log_info_fmt("Client UDP bound to {}:{}",
                     local_ep.address().to_string(), local_ep.port());

        // asio::ip::make_address 在 IP 格式非法时抛异常，需 try-catch 保护。
        asio::ip::address server_address;
        try {
            server_address = asio::ip::make_address(cfg.server_ip);
        } catch (const std::exception& e) {
            set_last_error(std::string("invalid server IP address '") + cfg.server_ip
                           + "': " + e.what());
            log_error_fmt("invalid server IP address '{}': {}", cfg.server_ip, e.what());
            grpc_client.disconnect(session_id);
            return SessionOutcome::Fatal;
        }
        const asio::ip::udp::endpoint server_udp_endpoint(server_address, connect_result.udp_port);

        // Init RingBuffer: JitterBuffer → RingBuffer → 播放线程。
        audio::SpscRingBuffer ringbuffer(rt_cfg.playback_ringbuffer_size);
        // 字节速率（B/ms），把容量换算成时长，便于直观比较缓冲余量。
        const double bytes_per_ms = static_cast<double>(server_audio_format.sample_rate)
                                  * server_audio_format.frame_bytes() / 1000.0;
        log_info_fmt("Playback RingBuffer: requested={} bytes ({:.1f}ms), actual={} bytes ({:.1f}ms)",
                     rt_cfg.playback_ringbuffer_size,
                     rt_cfg.playback_ringbuffer_size / bytes_per_ms,
                     ringbuffer.capacity(), ringbuffer.capacity() / bytes_per_ms);

        // 每包 PCM 参数。FRAMES_PER_PACKET 是固定帧数（与采样率无关）。
        const std::uint32_t frames_per_packet = config::AUDIO_FRAMES_PER_PACKET;
        const std::size_t packet_payload_size =
            static_cast<std::size_t>(frames_per_packet) * server_audio_format.frame_bytes();
        // 实际每包时长（微秒）。仅用于阈值比较，不参与逐包累加（JB 内部用纳秒）。
        const auto packet_duration_us = std::chrono::microseconds(
            static_cast<std::int64_t>(frames_per_packet) * 1'000'000
            / server_audio_format.sample_rate);

        const double packet_duration_ms =
            static_cast<double>(packet_duration_us.count()) / 1000.0;
        const std::size_t packet_wire_size =
            sizeof(net::AudioPacketHeader) + packet_payload_size;
        log_info_fmt("Audio packet: {} frames/packet ({:.2f}ms @ {}Hz), payload={}B, wire={}B",
                     frames_per_packet, packet_duration_ms,
                     server_audio_format.sample_rate, packet_payload_size, packet_wire_size);

        // ---- JB 单参数推导（用户面仅 jitter_buffer_ms，运行点全内部）----
        // capacity = bit_ceil(max(MIN_CAPACITY, ceil(ms→packets)))：2 的幂 ring。
        // 向上取整 + 2 的幂对齐 = 缓冲预算"至少"语义。
        const std::uint32_t jb_ms = rt_cfg.jitter_buffer_ms > 0
            ? rt_cfg.jitter_buffer_ms
            : config::DEFAULT_JITTER_BUFFER_MS;
        const std::uint64_t requested_frames =
            static_cast<std::uint64_t>(jb_ms) * server_audio_format.sample_rate / 1000;
        const std::uint64_t requested_packets =
            (requested_frames + frames_per_packet - 1) / frames_per_packet;
        std::size_t jitter_capacity = config::JITTER_MIN_CAPACITY_PACKETS;
        while (jitter_capacity < requested_packets) {
            jitter_capacity <<= 1;
        }
        // 分配策略（比例固定，见 config.h）：
        //   ceiling = cap/2（自适应上限；上半区留乱序余量）
        //   floor   = cap/4（起播点 + 自适应下限，AIMD 区间 [cap/4, cap/2]）
        const std::size_t jb_ceiling_packets = jitter_capacity / 2;
        const std::size_t jb_floor_packets = jitter_capacity / 4;

        log_info_fmt("JitterBuffer: buffer={}ms -> capacity={} packets ({:.2f}ms), "
                     "floor={} packets ({:.2f}ms), ceiling={} packets ({:.2f}ms), {}B",
                     jb_ms,
                     jitter_capacity, packet_duration_ms * jitter_capacity,
                     jb_floor_packets, packet_duration_ms * jb_floor_packets,
                     jb_ceiling_packets, packet_duration_ms * jb_ceiling_packets,
                     jitter_capacity * packet_payload_size);

        // JitterBuffer: packet 时间顺序 + jitter + loss。
        // 自适应 target 区间 [floor, ceiling]，检测窗口 drift rebase 与 AIMD 共用。
        // 检测窗口可调（--jitter-detect-window）：小窗口响应快，大窗口判定稳。
        const std::uint32_t detect_window_packets = rt_cfg.jitter_detect_window_packets > 0
            ? rt_cfg.jitter_detect_window_packets
            : config::JITTER_DETECT_WINDOW_PACKETS;
        log_info_fmt("JitterBuffer detect window: {} packets", detect_window_packets);
        jitter::AdaptiveTargetConfig adapt_cfg {};
        adapt_cfg.max_packets = jb_ceiling_packets;
        jitter::JitterBuffer jitter_buffer(
            server_audio_format,
            frames_per_packet,
            jb_floor_packets,
            jitter_capacity,
            detect_window_packets,
            config::JITTER_DRIFT_REBASE_LATE_COUNT,
            adapt_cfg);

        // UDP 握手状态。
        std::atomic<bool> hello_acked { false };

        // WASAPI playback 初始化标志：在 playback 启动前丢弃音频包。
        std::atomic<bool> playback_ready { false };

        std::atomic<std::int64_t> last_audio_recv_ns {
            std::chrono::steady_clock::now().time_since_epoch().count()
        };

        // 已播放样本累计（播放线程累加，主线程读，relaxed）。
        std::atomic<std::uint64_t> played_samples { 0 };

        // keepalive HELLO 连续未收到 ACK 计数 + 告警去重标志（io_context 单线程访问）。
        std::uint32_t consecutive_missed_acks = 0;
        bool keepalive_loss_warned = false;

        // M5: DiagnosticsManager
        diag::DiagnosticsManager diag_manager(
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
                // deadline 在过去时（追赶模式），用 1ms 最小间隔防止 busy-loop。
                const auto now = std::chrono::steady_clock::now();
                if (*deadline <= now) {
                    jb_timer.expires_after(std::chrono::milliseconds(1));
                } else {
                    jb_timer.expires_at(*deadline);
                }
            }

            jb_timer.async_wait([&](const asio::error_code& ec) {
                if (ec || shutdown_requested_.load(std::memory_order_relaxed)) {
                    return;
                }

                // 一次性 pop 所有已过 deadline 的包（Windows 定时器粒度 ~15ms 会滞后多个 deadline）。
                const auto now = std::chrono::steady_clock::now();

                while (!shutdown_requested_.load(std::memory_order_relaxed)) {
                    const auto dl = jitter_buffer.next_playout_deadline();
                    if (!dl || *dl > now) {
                        break;
                    }

                    // RingBuffer 没有空间时停止 pop，保留包在 JitterBuffer 中。
                    //（WASAPI 未启动时 RB 满属正常；长时间断流的 timeline reset
                    //  已下沉到 JitterBuffer::pop_next 内部，仅在真正 pop 时触发。）
                    if (ringbuffer.available_write() < packet_payload_size) {
                        break;
                    }

                    const auto lateness =
                        std::chrono::duration_cast<std::chrono::microseconds>(now - *dl);

                    if (lateness > packet_duration_us) {
                        diag_manager.record_deadline_miss();
                    }

                    (void)jitter_buffer.pop_next(
                        std::span<std::byte> { jb_pop_buf.data(), jb_pop_buf.size() });
                    ringbuffer.write(std::span<const std::byte> { jb_pop_buf.data(), packet_payload_size });
                }

                schedule_jb_pop();
            });
        };

        // ---- UDP 接收回调 ----
        transport.start_receive([&](const asio::ip::udp::endpoint& /*sender*/,
                                    std::span<const std::byte> data) {
            const auto type = net::peek_type(data);
            if (!type) {
                log_debug_fmt("UDP recv unknown packet type ({} bytes)", data.size());
                return;
            }

            if (*type == net::PacketType::HelloAck) {
                const auto ack = net::decode_hello(data);
                if (ack && ack->session_id == session_id) {
                    // 收到 ACK：重置保活丢 ACK 计数（io_context 线程独占，无并发）。
                    consecutive_missed_acks = 0;
                    keepalive_loss_warned = false;

                    const bool was_acked = hello_acked.exchange(true, std::memory_order_relaxed);
                    if (!was_acked) {
                        log_info("UDP HELLO_ACK received, channel established");
                        // 首个 HELLO_ACK 到达时立即启动 JitterBuffer 调度器。
                        asio::post(ioc, [&] { schedule_jb_pop(); });
                    }
                    diag_manager.record_hello_ack_received();
                    diag_manager.record_hello_ack();
                }
            } else if (*type == net::PacketType::Audio) {
                const auto decoded = net::decode_audio(data);
                if (decoded) {
                    // WASAPI playback 未就绪时丢弃音频包，不 push 到 JB。
                    if (!playback_ready.load(std::memory_order_relaxed)) {
                        return;
                    }

                    jitter_buffer.push(decoded->header.sequence, decoded->payload);

                    diag_manager.record_packet_arrival(decoded->header.sequence,
                                                       decoded->header.sample_position);
                    diag_manager.record_audio_bytes(decoded->payload.size());

                    last_audio_recv_ns.store(
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        std::memory_order_relaxed);
                } else {
                    log_debug_fmt("Failed to decode Audio packet ({} bytes)", data.size());
                }
            }
        });

        std::thread ioc_thread([&] {
            ioc.run();
        });

        // ---- 发送 HELLO 直到收到 HELLO_ACK 或超时 ----
        std::array<std::byte, sizeof(net::HelloPacket)> hello_buf {};
        const auto hello_written = net::encode_hello(session_id, hello_buf);

        int hello_attempts = 0;
        while (!shutdown_requested_.load(std::memory_order_relaxed)
               && !hello_acked.load(std::memory_order_relaxed)) {
            if (++hello_attempts > config::HELLO_HANDSHAKE_MAX_ATTEMPTS) {
                break;
            }
            log_debug_fmt("Sending HELLO attempt {}/{} to {}",
                          hello_attempts, config::HELLO_HANDSHAKE_MAX_ATTEMPTS,
                          cfg.server_ip);
            diag_manager.record_hello_sent();
            transport.send(server_udp_endpoint,
                           std::span<const std::byte> { hello_buf.data(), hello_written });
            std::this_thread::sleep_for(config::HELLO_HANDSHAKE_RETRY_INTERVAL);
        }

        if (!hello_acked.load(std::memory_order_relaxed)) {
            set_last_error("UDP HELLO_ACK timeout (server reachable but UDP handshake failed)");
            log_error_fmt("UDP HELLO_ACK timeout ({} attempts, {}ms)",
                          hello_attempts,
                          hello_attempts * config::HELLO_HANDSHAKE_RETRY_INTERVAL.count());
            transport.stop();
            ioc.stop();
            ioc_thread.join();
            grpc_client.disconnect(session_id);
            return SessionOutcome::Retryable;
        }

        // ---- WASAPI Playback ----
        auto playback = audio::create_playback_backend();
        if (!playback) {
            set_last_error("no audio playback backend available");
            log_error("no audio playback backend available");
            transport.stop();
            ioc.stop();
            ioc_thread.join();
            grpc_client.disconnect(session_id);
            return SessionOutcome::Fatal;
        }

        // 启动水位（pre-roll latch）：RB 是 1:1 直通管道，稳态占用 = 起跑点。
        // 若 fill 回调从空缓冲就开始消费，RB 永远在空附近运行，拉大容量无济于事。
        // 水位：首拍消费前 RB 需积累 capacity/2（16KB RB ≈ 21ms），此后闩锁放行。
        // pre-roll 等待期输出静音（backend 契约：fill 返回 0 的部分被 memset 静音），
        // 不计 underrun（是启动策略而非故障）。
        //
        // 闩锁重臂：断流把 RB 排干后（连续 starved_rearm_callbacks 次完全空仓），
        // 重新进入等待状态，恢复供水后重新蓄到水位再消费——运行点回到半水位，
        // 而非驻留在断流瞬间形成的低水位平衡（实测断流后 RB 会停在 4-10ms 运行，
        // 余量缩水易被下次抖动打穿）。去抖 3 次（~30ms）：批量 pop 的相位差最多
        // 让定时器滞后一拍（~15.6ms），不会连续 3 拍完全空仓，避免误触发。
        std::atomic<bool> preroll_done { false };
        std::atomic<std::uint32_t> starved_callbacks { 0 };
        const std::size_t preroll_watermark = ringbuffer.capacity() / 2;
        constexpr std::uint32_t starved_rearm_callbacks = 3;

        // 低水位看门狗：断流后的"部分饥饿"平衡（fill 拿到部分数据 got>0，
        // 完全空仓重臂永不触发）会让 RB 运行点驻留在低水位（实测 8-13ms，
        // min 4ms），余量持续暴露在下一轮抖动风险中。占用持续低于水位的
        // 75% 达 low_water_rearm_samples 次采样（500ms × 6 = 3s）时主动
        // 重臂闩锁——用一次性 ~水位差静音换回半水位锚点。干净期占用
        // min ≈ 水位（不会低于 75%）且波动插入高样本打断连续计数，无误触发。
        // 闩锁关闭期间（pre-preroll / 刚重臂）跳过，避免蓄水期重复触发。
        std::uint32_t low_water_streak = 0; // 仅主线程访问
        constexpr std::uint32_t low_water_rearm_samples = 6;
        const std::size_t low_watermark_bytes = preroll_watermark - preroll_watermark / 4;

        if (!playback->start(server_audio_format, [&](std::span<std::byte> out) -> std::size_t {
                // 水位检查：闩锁打开后零开销；重臂后再次生效。
                if (!preroll_done.load(std::memory_order_relaxed)) {
                    if (ringbuffer.available_read() < preroll_watermark) {
                        return 0; // 静音等待，不计 underrun，不计消费
                    }
                    log_info_fmt("Playback pre-roll complete: {} bytes buffered (watermark {})",
                                 ringbuffer.available_read(), preroll_watermark);
                    preroll_done.store(true, std::memory_order_relaxed);
                    starved_callbacks.store(0, std::memory_order_relaxed);
                }
                const auto got = ringbuffer.read(out);
                if (got < out.size()) {
                    diag_manager.record_underrun();
                    // 仅"完全空仓"计饥饿（部分填充说明供给未中断，不累加也不清零）。
                    if (got == 0
                        && starved_callbacks.fetch_add(1, std::memory_order_relaxed) + 1
                            >= starved_rearm_callbacks) {
                        preroll_done.store(false, std::memory_order_relaxed);
                        diag_manager.record_rb_rearm();
                        log_info_fmt("Playback buffer starved {} consecutive callbacks, "
                                     "re-arming pre-roll latch",
                                     starved_rearm_callbacks);
                    }
                } else {
                    starved_callbacks.store(0, std::memory_order_relaxed);
                }
                // 整个 out 缓冲都会被播放（含静音填充），累加已播放样本数。
                played_samples.fetch_add(out.size() / server_audio_format.frame_bytes(),
                                         std::memory_order_relaxed);
                return got;
            })) {
            set_last_error("failed to start audio playback (see log above for details)");
            log_error("failed to start audio playback (see log above for details)");
            transport.stop();
            ioc.stop();
            ioc_thread.join();
            grpc_client.disconnect(session_id);
            return SessionOutcome::Fatal;
        }

        log_info("Playback started with server audio format");
        playback_ready.store(true, std::memory_order_relaxed);
        set_state(ClientState::Playing);
        // 重置音频超时计时器：HELLO 握手 + playback 初始化可能消耗大部分
        // CLIENT_AUDIO_RECV_TIMEOUT，从 playback 就绪时刻重新计时。
        last_audio_recv_ns.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);

        // ---- UDP HELLO 保活定时器 ----
        asio::steady_timer keepalive_timer(ioc);
        std::function<void()> schedule_keepalive;
        schedule_keepalive = [&]() {
            keepalive_timer.expires_after(config::HELLO_KEEPALIVE_INTERVAL);
            keepalive_timer.async_wait([&](const asio::error_code& ec) {
                if (ec || shutdown_requested_.load(std::memory_order_relaxed)) {
                    return;
                }
                log_trace_fmt("HELLO keepalive sent to {}:{} (session=0x{:08X})",
                              cfg.server_ip, connect_result.udp_port, session_id);
                // 连续未收到 ACK 计数：早于音频超时暴露服务器已断。
                ++consecutive_missed_acks;
                if (!keepalive_loss_warned
                    && consecutive_missed_acks >= config::HELLO_ACK_WARN_THRESHOLD) {
                    keepalive_loss_warned = true;
                    log_warn_fmt("No HELLO_ACK for {} consecutive keepalives ({}s), server may be down",
                                 consecutive_missed_acks,
                                 consecutive_missed_acks * config::HELLO_KEEPALIVE_INTERVAL.count());
                }
                diag_manager.record_hello_sent();
                transport.send(server_udp_endpoint,
                               std::span<const std::byte> { hello_buf.data(), hello_written });
                schedule_keepalive();
            });
        };
        schedule_keepalive();

        // ---- 等待退出（主循环健康监控）----
        log_info("Client running. Press Ctrl+C to stop.");

        auto last_stats_time = std::chrono::steady_clock::now();
        auto last_rb_sample_time = last_stats_time;

        SessionOutcome outcome = SessionOutcome::CleanExit;
        while (!shutdown_requested_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(POLL_INTERVAL);

            if (!playback->is_running()) {
                log_error("Playback backend stopped unexpectedly, shutting down");
                set_last_error("Playback backend stopped unexpectedly");
                outcome = SessionOutcome::Fatal;
                break;
            }

            {
                const auto now = std::chrono::steady_clock::now();
                const auto last_ns = last_audio_recv_ns.load(std::memory_order_relaxed);
                const auto last_time = std::chrono::steady_clock::time_point(
                    std::chrono::steady_clock::duration(last_ns));
                if (now - last_time > config::CLIENT_AUDIO_RECV_TIMEOUT) {
                    set_last_error("no audio data from server (server may be down or UDP blocked)");
                    log_error_fmt("No audio data from server for {}s, server may be down",
                                  config::CLIENT_AUDIO_RECV_TIMEOUT.count());
                    outcome = SessionOutcome::Retryable;
                    break;
                }
            }

            const auto now = std::chrono::steady_clock::now();

            // 高频采样 RB 占用到 slope 窗口（与日志输出解耦）。
            if (now - last_rb_sample_time >= RB_SAMPLE_INTERVAL) {
                diag_manager.record_rb_occupancy();
                last_rb_sample_time = now;

                // 低水位看门狗（见声明处注释）：仅在闩锁打开（正常运行）时评估，
                // 蓄水期（首启动 / 刚重臂）跳过。
                if (preroll_done.load(std::memory_order_relaxed)) {
                    if (ringbuffer.available_read() < low_watermark_bytes) {
                        if (++low_water_streak >= low_water_rearm_samples) {
                            low_water_streak = 0;
                            preroll_done.store(false, std::memory_order_relaxed);
                            diag_manager.record_rb_rearm();
                            log_info_fmt("Playback RB low-watermark watchdog: occupancy {}B "
                                         "below {}B (75% of watermark) for {} samples, "
                                         "re-arming pre-roll latch to restore operating point",
                                         ringbuffer.available_read(), low_watermark_bytes,
                                         low_water_rearm_samples);
                        }
                    } else {
                        low_water_streak = 0;
                    }
                } else {
                    low_water_streak = 0;
                }
            }

            // 周期性诊断刷新：collect_and_log 输出日志并更新快照缓存
            //（diagnostics() 即时返回快照，刷新频率由该常量决定，见 config.h）。
            if (now - last_stats_time >= config::DIAGNOSTICS_REFRESH_INTERVAL) {
                diag_manager.collect_and_log(jitter_buffer);
                // 同步最新快照到缓存，供外部 diagnostics() 读取（跨线程用 mutex）。
                {
                    std::lock_guard<std::mutex> lock(diag_mutex_);
                    last_diag_snapshot_ = diag_manager.snapshot();
                }
                last_stats_time = now;
            }
        }

        log_info("Shutting down...");
        playback->stop();

        // 先通知 server 移除 session 并停止发包，再关闭本地 UDP（避免 ICMP 风暴）。
        grpc_client.disconnect(session_id);

        transport.stop();
        ioc.stop();

        if (ioc_thread.joinable()) {
            ioc_thread.join();
        }

        return outcome;
    }

    // ---- 会话循环：串起单次会话 + 指数退避重连 ----
    void session_loop()
    {
        int attempt = 0;
        auto session_start = std::chrono::steady_clock::now();

        while (!shutdown_requested_.load(std::memory_order_relaxed)) {
            set_state(ClientState::Connecting);
            const auto outcome = run_one_session();

            if (shutdown_requested_.load(std::memory_order_relaxed)) {
                break;
            }

            if (outcome == SessionOutcome::Fatal) {
                set_state(ClientState::Failed);
                if (cb.on_error) {
                    // 加锁拷贝，避免与主线程 last_error() 并发读同一 string。
                    std::string err;
                    {
                        std::lock_guard<std::mutex> lock(error_mutex_);
                        err = last_error_;
                    }
                    cb.on_error(std::move(err));
                }
                break;
            }

            // 非致命退出（CleanExit 或 Retryable）：
            if (!cfg.auto_reconnect) {
                // 非重连模式：自然退出（关闭请求或服务端已断，均非致命）。
                set_state(ClientState::Stopped);
                break;
            }

            set_state(ClientState::Reconnecting);

            // 上次会话稳定运行过（>= RECONNECT_BACKOFF_RESET_AFTER）则重置退避。
            const auto session_duration = std::chrono::steady_clock::now() - session_start;
            if (session_duration >= config::RECONNECT_BACKOFF_RESET_AFTER) {
                attempt = 0;
            }

            const int exp = std::min(attempt, 5); // 2^5 = 32s，再往上封顶
            auto delay = std::chrono::seconds(1 << exp);
            if (delay > config::RECONNECT_MAX_DELAY) {
                delay = config::RECONNECT_MAX_DELAY;
            }
            log_info_fmt("Reconnecting in {}s (attempt {})", delay.count(), attempt + 1);
            ++attempt;

            // 分段 sleep，便于及时响应关闭请求。
            const auto deadline = std::chrono::steady_clock::now() + delay;
            while (!shutdown_requested_.load(std::memory_order_relaxed)
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(POLL_INTERVAL);
            }
            session_start = std::chrono::steady_clock::now();
        }

        // 终端状态：若因关闭请求退出且尚未到达终态，标记 Stopped。
        const auto st = state_.load(std::memory_order_relaxed);
        if (shutdown_requested_.load(std::memory_order_relaxed)
            && st != ClientState::Failed && st != ClientState::Stopped) {
            set_state(ClientState::Stopped);
        }

        running_.store(false, std::memory_order_relaxed);
        if (cb.on_stopped) {
            cb.on_stopped();
        }
    }
};

ClientRuntime::ClientRuntime()
    : impl_(std::make_unique<Impl>())
{
}

ClientRuntime::~ClientRuntime()
{
    if (impl_->session_thread.joinable()) {
        impl_->shutdown_requested_.store(true, std::memory_order_relaxed);
        impl_->session_thread.join();
    }
}

bool ClientRuntime::start(const ClientConfig& cfg, ClientCallbacks cb)
{
    Impl& p = *impl_;
    if (p.running_.load(std::memory_order_relaxed)) {
        return false; // 已在运行
    }

    p.cfg = cfg;
    p.cb = std::move(cb);
    p.last_error_.clear();
    p.shutdown_requested_.store(false, std::memory_order_relaxed);
    p.state_.store(ClientState::Idle, std::memory_order_relaxed);

    p.running_.store(true, std::memory_order_relaxed);
    p.session_thread = std::thread([&p] {
        p.session_loop();
    });
    return true;
}

void ClientRuntime::run(std::function<bool()> stop_when)
{
    Impl& p = *impl_;
    if (!p.running_.load(std::memory_order_relaxed)) {
        return; // 未启动或已结束
    }

    // 监控会话状态，直到：关闭请求 / stop_when / 终端状态。
    while (p.running_.load(std::memory_order_relaxed)
           && !p.shutdown_requested_.load(std::memory_order_relaxed)) {
        if (stop_when && stop_when()) {
            p.shutdown_requested_.store(true, std::memory_order_relaxed);
            break;
        }
        const auto st = p.state_.load(std::memory_order_relaxed);
        if (st == ClientState::Stopped || st == ClientState::Failed) {
            break;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }

    if (p.session_thread.joinable()) {
        p.session_thread.join();
    }
}

void ClientRuntime::shutdown() noexcept
{
    impl_->shutdown_requested_.store(true, std::memory_order_relaxed);
}

bool ClientRuntime::is_running() const noexcept
{
    return impl_->running_.load(std::memory_order_relaxed);
}

ClientState ClientRuntime::state() const noexcept
{
    return impl_->state_.load(std::memory_order_relaxed);
}

std::string ClientRuntime::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->error_mutex_);
    return impl_->last_error_;
}

std::optional<diag::DiagnosticsManager::Snapshot> ClientRuntime::diagnostics() const
{
    std::lock_guard<std::mutex> lock(impl_->diag_mutex_);
    return impl_->last_diag_snapshot_;
}

std::optional<AudioFormat> ClientRuntime::audio_format() const
{
    std::lock_guard<std::mutex> lock(impl_->diag_mutex_);
    return impl_->audio_format_;
}

} // namespace aqua::client
