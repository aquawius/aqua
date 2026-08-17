#include "core/server/server_runtime.h"

#include "core/audio/backend/audio_backend_factory.h"
#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/grpc/grpc_server.h"
#include "core/logger/logger.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/session/session_manager.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>

namespace aqua::server {

namespace {

// gRPC 启动探测：最多轮询 500ms（50 次 × 10ms），与旧 CLI 行为一致。
constexpr int GRPC_START_POLL_COUNT = 50;
constexpr auto GRPC_START_POLL_INTERVAL = std::chrono::milliseconds(10);

// 主循环健康监控轮询间隔：兼顾响应速度与 CPU 开销。
constexpr auto MONITOR_POLL_INTERVAL = std::chrono::milliseconds(50);

} // namespace

struct ServerRuntime::Impl {
    ServerConfig cfg;
    ServerCallbacks cb;

    // 关闭标志：shutdown() 置位，run() 主循环与各线程轮询。
    // 用 atomic 让任意线程（含信号处理函数）安全读取/写入。
    std::atomic<bool> shutdown_requested_ { false };
    std::atomic<bool> running_ { false };
    // start() 是否已成功完成。用于拒绝重复 start()（避免覆盖仍在运行的子系统）。
    bool started_ = false;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    // ---- 音频采集 ----
    std::unique_ptr<audio::CaptureBackend> capture;
    std::unique_ptr<audio::SpscRingBuffer> ringbuffer;
    // capture 回调（音频线程）写入、packetizer（sender 线程）读取，用 atomic。
    std::atomic<std::uint64_t> capture_dropped_bytes { 0 };
    // capture → packetizer 数据就绪通知（counting_semaphore，默认上限近似无界）。
    std::counting_semaphore<> capture_sem { 0 };
    AudioFormat capture_format {};

    // ---- 控制面 ----
    SessionManager sessions;

    // ---- gRPC ----
    std::unique_ptr<grpc::GrpcServer> grpc_server;

    // ---- 数据面 ----
    // ioc 必须先于 transport 析构（transport 持有 ioc 引用与 socket）。
    asio::io_context ioc;
    std::unique_ptr<net::UdpTransport> transport;
    asio::steady_timer cleanup_timer { ioc };

    // ---- 线程 ----
    std::thread grpc_thread;
    std::thread ioc_thread;
    std::thread sender_thread;

    void set_last_error(std::string message)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_ = std::move(message);
    }

    void report_error(std::string message)
    {
        log_error(message);
        set_last_error(message);
        if (cb.on_error) {
            cb.on_error(std::move(message));
        }
    }

    // ---- UDP 接收路由（io_context 线程）----
    void handle_udp_receive(const asio::ip::udp::endpoint& sender,
                            std::span<const std::byte> data)
    {
        auto type = net::peek_type(data);
        if (!type) {
            log_debug_fmt("UDP recv unknown packet type from {}:{} ({} bytes)",
                          sender.address().to_string(), sender.port(), data.size());
            return;
        }

        if (*type == net::PacketType::Hello) {
            auto hello = net::decode_hello(data);
            if (!hello) {
                return;
            }
            // HELLO 兼任两种角色：
            //   1. 首次握手（Created -> Connected）
            //   2. UDP keepalive（已 Connected，刷新 NAT 映射 + last_seen）
            const bool was_connected = sessions.is_connected(hello->session_id);
            if (sessions.establish_udp(hello->session_id, sender)) {
                if (!was_connected) {
                    log_info_fmt("Session 0x{:08X} UDP established: {}:{}",
                                 hello->session_id,
                                 sender.address().to_string(), sender.port());
                } else {
                    log_trace_fmt("Session 0x{:08X} HELLO keepalive from {}:{}",
                                  hello->session_id,
                                  sender.address().to_string(), sender.port());
                }
                // 始终回复 HELLO_ACK（首次握手需要，keepalive 也可用于确认链路）。
                std::array<std::byte, sizeof(net::HelloPacket)> ack_buf {};
                net::encode_hello_ack(hello->session_id, ack_buf);
                transport->send(sender,
                                std::span<const std::byte> { ack_buf.data(), ack_buf.size() });
            } else {
                log_warn_fmt("HELLO from unknown session 0x{:08X} (from {}:{})",
                             hello->session_id,
                             sender.address().to_string(), sender.port());
            }
        } else if (*type == net::PacketType::Audio) {
            // 当前为单向音频（server -> client），server 不应收到 Audio 包。
            // 若收到（恶意/bug client），直接丢弃，不 touch_session —— 否则
            // client 持续发 Audio 包会让它的 session 永不过期。
            log_debug_fmt("Server received unexpected Audio packet from {}:{} ({} bytes), dropping",
                          sender.address().to_string(), sender.port(), data.size());
        }
    }

    // ---- session 超时清理定时器（挂 io_context，替代独立线程）----
    void schedule_cleanup()
    {
        cleanup_timer.expires_after(config::SESSION_CLEANUP_INTERVAL);
        cleanup_timer.async_wait([this](const asio::error_code& ec) {
            if (ec || shutdown_requested_.load(std::memory_order_relaxed)) {
                return;
            }
            const auto expired = sessions.collect_expired_sessions(config::SESSION_TIMEOUT);
            for (const auto id : expired) {
                log_info_fmt("Session 0x{:08X} expired, removing", id);
                sessions.remove_session(id);
            }
            schedule_cleanup();
        });
    }

    // ---- packetizer 线程：RingBuffer → 编码 → 广播 ----
    void sender_loop()
    {
        // FRAMES_PER_PACKET 是固定帧数（与采样率无关），packet_duration 由它推导，
        // 任何采样率下都精确等于音频内容真实时长，无截断漂移。
        const std::uint32_t frames_per_packet = config::AUDIO_FRAMES_PER_PACKET;
        const std::size_t packet_payload_size =
            frames_per_packet * capture_format.frame_bytes();
        const std::size_t send_buf_size = sizeof(net::AudioPacketHeader) + packet_payload_size;

        const double packet_duration_ms =
            static_cast<double>(frames_per_packet) * 1000.0 / capture_format.sample_rate;
        log_info_fmt("Packetizer: {} frames/packet ({:.2f}ms), payload={}B, wire={}B",
                     frames_per_packet, packet_duration_ms, packet_payload_size, send_buf_size);

        std::vector<std::byte> send_buf(send_buf_size);
        std::vector<std::byte> pcm_buf(packet_payload_size);

        std::uint32_t sequence = 0;
        std::uint64_t sample_position = 0;

        // 周期性统计日志（每 5 秒输出一次）。
        constexpr auto STATS_INTERVAL = std::chrono::seconds(5);
        auto last_stats_time = std::chrono::steady_clock::now();
        std::uint64_t stats_packets = 0;
        std::uint64_t stats_bytes = 0;
        std::uint64_t stats_pcm_bytes = 0;

        while (!shutdown_requested_.load(std::memory_order_relaxed)) {
            // 从 RingBuffer 读取一包数据。
            // 3ms packet + 10ms WASAPI capture 天然产生跨 callback 的残余数据，
            // 残余数据必须跨 WASAPI callback 保留，不能因 RingBuffer 短暂为空就丢弃。
            std::size_t got = 0;
            while (got < pcm_buf.size()
                   && !shutdown_requested_.load(std::memory_order_relaxed)) {
                got += ringbuffer->read(std::span<std::byte> {
                    pcm_buf.data() + got, pcm_buf.size() - got });
                if (got < pcm_buf.size()) {
                    // 数据不足：阻塞等待 capture 回调通知（OS 事件，非 busy-loop）。
                    // 100ms 超时仅用于定期检查关闭标志。
                    (void)capture_sem.try_acquire_for(std::chrono::milliseconds(100));
                }
            }
            if (shutdown_requested_.load(std::memory_order_relaxed)) {
                break;
            }

            // 编码音频包（session_id=0 表示广播到所有已连接 session）。
            const auto written = net::encode_audio(
                net::kBroadcastSessionId,
                sequence,
                static_cast<std::uint32_t>(sample_position),
                std::span<const std::byte> { pcm_buf.data(), got },
                std::span<std::byte> { send_buf.data(), send_buf.size() });

            if (written > 0) {
                std::size_t recipients = 0;
                sessions.for_each_connected([&](auto /*id*/, const auto& endpoint) {
                    transport->send(endpoint,
                                    std::span<const std::byte> { send_buf.data(), written });
                    ++recipients;
                    return true; // 继续遍历
                });
                ++stats_packets;
                stats_bytes += written;
                stats_pcm_bytes += got;
                ++sequence;
                sample_position += frames_per_packet;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= STATS_INTERVAL) {
                const auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - last_stats_time).count();
                const auto session_count = sessions.session_count();
                const auto dropped = capture_dropped_bytes.exchange(0, std::memory_order_relaxed);
                log_debug_fmt("Packetizer stats: {} packets, {:.1f} KB in {:.2f}s ({:.1f} packets/s), {} active session(s), pcm={:.1f} KB, dropped={:.1f} KB",
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
    }

    // ---- 完整优雅关闭（run() 主循环退出后调用）----
    void teardown()
    {
        shutdown_requested_.store(true, std::memory_order_relaxed);
        if (capture) {
            capture->stop();
        }
        if (grpc_server) {
            grpc_server->shutdown();
        }
        if (transport) {
            transport->stop();
        }
        ioc.stop();

        if (grpc_thread.joinable()) {
            grpc_thread.join();
        }
        if (ioc_thread.joinable()) {
            ioc_thread.join();
        }
        if (sender_thread.joinable()) {
            sender_thread.join();
        }

        // 清理残留 session（如 client 仍在线但 server 被强制关闭），避免析构 warning。
        sessions.clear();

        running_.store(false, std::memory_order_relaxed);
        if (cb.on_stopped) {
            cb.on_stopped();
        }
    }
};

ServerRuntime::ServerRuntime()
    : impl_(std::make_unique<Impl>())
{
}

ServerRuntime::~ServerRuntime()
{
    // 若调用方忘记 shutdown()/run() 收尾，这里兜底关闭。
    if (impl_->running_.load(std::memory_order_relaxed)) {
        impl_->teardown();
    }
}

bool ServerRuntime::start(const ServerConfig& cfg, ServerCallbacks cb)
{
    Impl& p = *impl_;
    if (p.started_) {
        p.set_last_error("server already started");
        return false;
    }
    p.cfg = cfg;
    p.cb = std::move(cb);
    p.last_error_.clear();

    log_info_fmt("Starting Aqua server on {} gRPC={}, UDP={}",
                 cfg.bind_ip, cfg.rpc_port, cfg.udp_port);

    // ---- WASAPI Loopback Capture（先启动，获取 AudioFormat 给 gRPC）----
    // 启动顺序：WASAPI -> gRPC(控制面) -> UDP(数据面) -> 其余线程。
    // 失败路径：任何步骤失败时，之前已启动的资源按逆序清理。
    p.capture = audio::create_capture_backend();
    if (!p.capture) {
        p.set_last_error("no audio capture backend available");
        return false;
    }

    p.ringbuffer = std::make_unique<audio::SpscRingBuffer>(cfg.runtime.capture_ringbuffer_size);

    if (!p.capture->start([&p](std::span<const std::byte> pcm) {
            const auto written = p.ringbuffer->write(pcm);
            if (written < pcm.size()) {
                p.capture_dropped_bytes.fetch_add(pcm.size() - written,
                                                  std::memory_order_relaxed);
            }
            p.capture_sem.release(); // 立即唤醒 packetizer 线程
        }, p.capture_format)) {
        p.set_last_error("failed to start audio capture");
        return false;
    }

    log_info_fmt("Capture format: {}ch {}Hz encoding={}",
                 p.capture_format.channels, p.capture_format.sample_rate,
                 static_cast<int>(p.capture_format.encoding));

    // 字节速率（B/ms），把 RingBuffer 容量换算成时长。
    const double capture_bytes_per_ms = static_cast<double>(p.capture_format.sample_rate)
                                      * p.capture_format.frame_bytes() / 1000.0;
    log_info_fmt("Capture RingBuffer: requested={} bytes ({:.1f}ms), actual={} bytes ({:.1f}ms)",
                 cfg.runtime.capture_ringbuffer_size,
                 cfg.runtime.capture_ringbuffer_size / capture_bytes_per_ms,
                 p.ringbuffer->capacity(), p.ringbuffer->capacity() / capture_bytes_per_ms);

    // ---- gRPC Server（控制面先就绪，client 可先 Connect 拿到 session_id）----
    p.grpc_server = std::make_unique<grpc::GrpcServer>(
        p.sessions, p.capture_format,
        cfg.bind_ip, cfg.rpc_port,
        cfg.bind_ip, cfg.udp_port);

    p.grpc_thread = std::thread([&p] {
        p.grpc_server->run();
    });

    // 检测 gRPC 是否成功启动；失败则回滚清理。
    {
        bool grpc_ok = false;
        for (int i = 0; i < GRPC_START_POLL_COUNT; ++i) {
            if (p.grpc_server->is_running()) {
                grpc_ok = true;
                break;
            }
            std::this_thread::sleep_for(GRPC_START_POLL_INTERVAL);
        }
        if (!grpc_ok) {
            p.set_last_error("failed to start gRPC server on " + std::to_string(cfg.rpc_port));
            p.grpc_server->shutdown();
            if (p.grpc_thread.joinable()) {
                p.grpc_thread.join();
            }
            p.capture->stop();
            return false;
        }
    }

    // ---- UDP Transport（数据面）----
    p.transport = std::make_unique<net::UdpTransport>(p.ioc);
    if (!p.transport->bind(cfg.bind_ip, cfg.udp_port)) {
        p.set_last_error("failed to bind UDP port " + std::to_string(cfg.udp_port));
        p.grpc_server->shutdown();
        if (p.grpc_thread.joinable()) {
            p.grpc_thread.join();
        }
        p.capture->stop();
        return false;
    }
    log_info_fmt("UDP bound to {}:{}", cfg.bind_ip, cfg.udp_port);

    p.transport->start_receive([&p](const asio::ip::udp::endpoint& sender,
                                    std::span<const std::byte> data) {
        p.handle_udp_receive(sender, data);
    });

    p.ioc_thread = std::thread([&p] {
        p.ioc.run();
    });

    p.schedule_cleanup();

    p.sender_thread = std::thread([&p] {
        p.sender_loop();
    });

    p.running_.store(true, std::memory_order_relaxed);
    p.started_ = true;
    if (p.cb.on_started) {
        p.cb.on_started();
    }
    return true;
}

void ServerRuntime::run(std::function<bool()> stop_when)
{
    Impl& p = *impl_;
    if (!p.running_.load(std::memory_order_relaxed)) {
        return; // start() 失败或已经停止
    }

    log_info("Server running. Press Ctrl+C to stop.");

    // 健康监控主循环：50ms 轮询。检测到外部停止请求或致命错误时退出并清理。
    while (!p.shutdown_requested_.load(std::memory_order_relaxed)) {
        if (stop_when && stop_when()) {
            p.shutdown_requested_.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(MONITOR_POLL_INTERVAL);

        // 采集后端健康：初始化成功后线程异常退出（设备被禁用/移除）→ 优雅退出。
        if (p.capture && !p.capture->is_running()) {
            p.report_error("Capture backend stopped unexpectedly, shutting down");
            break;
        }
        // gRPC 健康：线程异常退出（端口被占用后 Wait 立即返回等）→ 优雅退出。
        if (p.grpc_server && !p.grpc_server->is_running()) {
            p.report_error("gRPC server stopped unexpectedly, shutting down");
            break;
        }
    }

    p.teardown();
}

void ServerRuntime::shutdown() noexcept
{
    impl_->shutdown_requested_.store(true, std::memory_order_relaxed);
}

bool ServerRuntime::is_running() const noexcept
{
    return impl_->running_.load(std::memory_order_relaxed);
}

std::string ServerRuntime::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->error_mutex_);
    return impl_->last_error_;
}

} // namespace aqua::server
