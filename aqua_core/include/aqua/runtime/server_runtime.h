#ifndef AQUA_RUNTIME_SERVER_RUNTIME_H
#define AQUA_RUNTIME_SERVER_RUNTIME_H

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/capture/capture_manager.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/audio/queue/audio_frame_queue.h"
#include "aqua/diagnostics/server_diagnostics_snapshot.h"
#include "aqua/net/grpc/grpc_server.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/runtime/audio_network_dispatcher.h"
#include "aqua/runtime/runtime_config.h"
#include "aqua/runtime/runtime_state.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace aqua::runtime {

// 音频格式在构造时一次性解析（config.format 优先，否则用后端默认格式），
// 会话内不可变（Format immutable）。采集设备运行期可切换：设备故障/默认
// 设备变化由 CaptureManager 按候选链重建采集端点（capture_switching_design.md），
// 会话格式、packetizer 与时间线不受影响。
struct ServerRuntimeConfig {
    // nullopt = use capture backend shared-mode default format.
    std::optional<audio::AudioFormat> format;
    std::uint32_t frame_count = 0;
    std::string server_ip = config::DEFAULT_BIND_IP;
    std::uint16_t udp_port = config::DEFAULT_UDP_PORT;
    std::chrono::milliseconds session_timeout { aqua::config::SESSION_TIMEOUT };
    std::chrono::milliseconds session_reap_interval { aqua::config::SESSION_REAP_INTERVAL };
    std::uint32_t network_queue_slots = config::DEFAULT_SERVER_NETWORK_QUEUE_SLOTS;
    audio::AudioCaptureConfig capture {
        .source = audio::AudioCaptureSource::OUTPUT_LOOPBACK,
        .device = std::nullopt,
        .format = std::nullopt,
        .frames_per_buffer = 0,
    };
    std::uint16_t rpc_port = config::DEFAULT_RPC_PORT;
    // Server 对 gRPC/UDP 使用同一个本地监听地址。advertised_udp_address / port
    // 是独立的通告地址，供 client 连接数据面使用；留空表示跟随 server_ip / udp_port。
    std::string advertised_udp_address;
    std::optional<std::uint16_t> advertised_udp_port;
};

class ServerRuntime final : public std::enable_shared_from_this<ServerRuntime> {
public:
    ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 生命周期是一次性的：start() / stop() 内部串行化；stop() 可安全地从其它
    // 控制线程并发调用，但会等待当前 start() 完成后再执行 teardown。ServerRuntime
    // 必须由 std::shared_ptr 持有，因为 reap 定时器使用 weak_from_this()。
    bool start();
    void stop() noexcept;

    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_ ? sessions_->session_count() : 0; }
    [[nodiscard]] RuntimeState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] audio::AudioError last_audio_error() const noexcept
    {
        return last_audio_error_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept { return dispatcher_.frames_encoded(); }
    [[nodiscard]] std::uint64_t frames_broadcast() const noexcept { return dispatcher_.frames_broadcast(); }
    [[nodiscard]] std::uint64_t frames_without_clients() const noexcept
    {
        return dispatcher_.frames_without_clients();
    }
    [[nodiscard]] std::uint64_t encode_failures() const noexcept
    {
        return dispatcher_.encode_failures();
    }
    [[nodiscard]] std::uint64_t dispatch_failures() const noexcept
    {
        return dispatcher_.dispatch_failures();
    }
    [[nodiscard]] std::uint64_t udp_hello_received() const noexcept { return udp_.hello_received(); }
    [[nodiscard]] std::uint64_t udp_hello_rejected() const noexcept { return udp_.hello_rejected(); }
    [[nodiscard]] std::uint64_t udp_sessions_established() const noexcept { return udp_.sessions_established(); }
    [[nodiscard]] std::uint64_t udp_sessions_refreshed() const noexcept { return udp_.sessions_refreshed(); }
    [[nodiscard]] std::uint64_t udp_hello_ack_attempts() const noexcept { return udp_.hello_ack_attempts(); }
    [[nodiscard]] std::uint64_t udp_malformed_datagrams() const noexcept { return udp_.malformed_datagrams(); }
    [[nodiscard]] std::uint64_t udp_non_hello_datagrams() const noexcept { return udp_.non_hello_datagrams(); }
    [[nodiscard]] net::UdpTransportStats udp_stats() const noexcept
    {
        return udp_.stats();
    }
    [[nodiscard]] std::uint16_t udp_port() const noexcept
    {
        return udp_.local_endpoint().port();
    }
    [[nodiscard]] std::uint64_t frames_dropped_before_network() const noexcept
    {
        return dispatcher_.dropped_frames();
    }
    [[nodiscard]] std::uint64_t packetizer_rejected_unaligned_blocks() const noexcept
    {
        return packetizer_.rejected_unaligned_blocks();
    }
    [[nodiscard]] std::uint64_t queue_accepted_frames() const noexcept { return frame_queue_.accepted_frames(); }
    [[nodiscard]] std::uint64_t queue_consumed_frames() const noexcept { return frame_queue_.consumed_frames(); }
    [[nodiscard]] std::uint64_t queue_dropped_frames() const noexcept { return frame_queue_.dropped_frames(); }
    [[nodiscard]] std::uint32_t queue_depth() const noexcept { return frame_queue_.size_slots(); }
    [[nodiscard]] session::SessionManager::Stats session_stats() const noexcept { return sessions_->stats(); }
    [[nodiscard]] std::uint64_t packetizer_input_blocks() const noexcept { return packetizer_.input_blocks(); }
    [[nodiscard]] audio::AudioCaptureStats capture_stats() const noexcept
    {
        return capture_manager_ ? capture_manager_->stats() : audio::AudioCaptureStats { };
    }
    [[nodiscard]] std::uint64_t packetizer_input_bytes() const noexcept { return packetizer_.input_bytes(); }
    [[nodiscard]] std::uint64_t packetizer_frames_emitted() const noexcept { return packetizer_.frames_emitted(); }
    [[nodiscard]] std::uint64_t dispatcher_published_frames() const noexcept { return dispatcher_.published_frames(); }
    [[nodiscard]] std::uint64_t dispatcher_worker_wakeups() const noexcept { return dispatcher_.worker_wakeups(); }

    [[nodiscard]] const audio::AudioFormat& audio_format() const noexcept { return effective_format_; }
    [[nodiscard]] std::uint32_t frame_count() const noexcept { return effective_frame_count_; }

    [[nodiscard]] bool capture_running() const noexcept
    {
        return capture_manager_ != nullptr && capture_manager_->is_running();
    }

    // capture 切换决策的执行结果（capture_switching_design.md §6 决策表）。
    enum class CaptureServiceAction : std::uint8_t {
        None, // 无事发生（无待处理错误、默认设备未变化）
        Restarted, // 执行了 restart 事务且成功（Running）
        Fatal, // capture 链耗尽 / 重试预算超限：调用方应 stop() 终止会话
    };

    // capture 切换服务（由 CLI control timer 每 500ms 在 ioc 线程驱动；
    // 经 lifecycle_mutex_ 与 stop()/start() 串行化）。决策表：
    //   - capture Fatal            -> 返回 Fatal（调用方 stop）；
    //   - 设备错误待处理           -> CaptureManager::restart_on_error
    //     （路由推导目标 + 候选链 + 共享重试预算），成功后清零锁存错误；
    //   - 否则                     -> CaptureManager::tick()（FollowSystem
    //     轮询系统默认设备变化并跟随，与错误驱动共享预算）；事务成功后
    //     吸收待处理错误标志并返回 Restarted（同一次设备变化只结算一次，
    //     防旧流临终错误引发二次 restart）。
    // 事务期间（Switching）到达的错误事件是旧流滞留错误，由
    // on_capture_event 的 Switching gate 直接丢弃，不进入本决策表。
    // restart 事务（stop/join/start）在本调用内同步完成；期间 packetizer
    // 无生产者，client 感知为一次普通网络抖动（JB 饥饿路径吸收）。
    CaptureServiceAction service_capture_switching() noexcept;

    // 一次性聚合诊断快照（字段契约见 aqua/diagnostics/server_diagnostics_snapshot.h）。
    // CLI 日志与 C API / GUI 前端共用；各字段为原子近似读值，任意线程可调用。
    [[nodiscard]] aqua::diagnostics::ServerDiagnosticsSnapshot take_diagnostics_snapshot() const noexcept;

private:
    struct ReapState;

    void on_capture_block(const audio::AudioBlock& block) noexcept;
    void on_capture_event(audio::AudioError error) noexcept;
    static void schedule_reap(const std::shared_ptr<ReapState>& reap,
        const std::weak_ptr<ServerRuntime>& weak_self,
        std::chrono::milliseconds interval, std::chrono::milliseconds timeout);
    void stop_locked() noexcept;
    bool enter_starting() noexcept;
    bool enter_stopping() noexcept;
    void enter_stopped() noexcept;

    ServerRuntimeConfig config_;
    asio::io_context& ioc_;
    std::unique_ptr<audio::AudioDeviceManager> device_mgr_;
    // capture 生命周期管理边界（capture_switching_design.md §3）：设备故障 /
    // 默认设备变化时按候选链重建采集端点，会话与时间线不受影响。
    // start() 内创建（晚于 device_mgr_）；stop_locked 停止。
    std::unique_ptr<audio::CaptureManager> capture_manager_;
    // Capture device is resolved exactly once during construction, but ONLY for
    // format probing (packetizer/queue geometry must be fixed before start).
    // The runtime capture route derives from config.capture (FollowSystem /
    // PreferredDevice); restart re-resolves devices per candidate.
    // NOTE: must be declared before effective_format_ — resolve_effective_format reads
    // effective_capture_device_ during construction (members initialize in declaration order).
    std::optional<audio::AudioDeviceId> effective_capture_device_;
    audio::AudioFormat effective_format_;
    std::uint32_t effective_frame_count_ = 0;
    std::uint32_t effective_network_queue_slots_ = 0;
    std::shared_ptr<session::SessionManager> sessions_;
    net::UdpServer udp_;
    audio::AudioPacketizer packetizer_;
    audio::AudioFrameQueue frame_queue_;
    AudioNetworkDispatcher dispatcher_;
    std::unique_ptr<grpc::GrpcServer> grpc_;
    std::thread grpc_thread_;
    struct ReapState {
        using Strand = asio::strand<asio::io_context::executor_type>;
        explicit ReapState(asio::io_context& ioc)
            : strand(asio::make_strand(ioc))
            , timer(std::make_shared<asio::steady_timer>(strand))
        {
        }
        Strand strand;
        std::shared_ptr<asio::steady_timer> timer;
    };
    std::shared_ptr<ReapState> reap_state_;
    mutable std::mutex lifecycle_mutex_;
    std::atomic<RuntimeState> state_ { RuntimeState::Created };
    std::atomic<audio::AudioError> last_audio_error_ { audio::AudioError::None };
    // capture 设备错误待处理标志（capture_switching_design.md §6 路径 1）：
    // on_capture_event（backend event 线程）只置位，restart 事务由
    // service_capture_switching（control tick，ioc 线程）执行——stop/join/start
    // 不得在回调线程执行。
    std::atomic<bool> capture_device_error_pending_ { false };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
