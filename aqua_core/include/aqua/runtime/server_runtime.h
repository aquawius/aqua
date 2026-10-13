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
#include "aqua/net/grpc/grpc_config.h"
#include "aqua/net/grpc/grpc_server.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/runtime/audio_network_dispatcher.h"
#include "aqua/runtime/runtime_config.h"
#include "aqua/runtime/runtime_state.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
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
    // 控制面（gRPC）本地绑定地址。通配时 gRPC 自身双栈 ⇒ 两族客户端都能连上控制面。
    // 它**不**决定数据面的族（见 udp_ipv4 / udp_ipv6）。
    std::string rpc_ip = config::DEFAULT_BIND_IP;
    std::uint16_t rpc_port = config::DEFAULT_RPC_PORT;
    // 数据面逐族绑定地址（IP 字面量，IPv6 可带 zone id）。**空字符串 = 关闭该族
    // 数据面**（CLI 的 `none`）。注意与 advertised_udp_* 的空值语义不同：
    // 那里空 = 跟随本族绑定地址。
    std::string udp_ipv4 = config::DEFAULT_UDP_IPV4;
    std::string udp_ipv6 = config::DEFAULT_UDP_IPV6;
    // 两族共用的数据面端口。必须 > 0：临时端口一旦被通告出去就是错的
    // （本层校验，不依赖 CLI 是否拦过）。
    std::uint16_t udp_port = config::DEFAULT_UDP_PORT;
    std::chrono::milliseconds session_timeout { aqua::config::SESSION_TIMEOUT };
    std::chrono::milliseconds session_reap_interval { aqua::config::SESSION_REAP_INTERVAL };
    std::uint32_t audio_queue_capacity_slots = config::DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS;
    audio::AudioCaptureConfig capture {
        .source = audio::AudioCaptureSource::OUTPUT_LOOPBACK,
        .route = audio::AudioRoute::follow_system(),
        .format = std::nullopt,
        .frames_per_buffer = 0,
    };
    // 逐族通告地址（连接 ConnectResponse 的 udp / udp6 槽）。留空表示跟随对应族的
    // 绑定地址；绑定地址为通配时下发**通配 sentinel**（客户端改用自己连控制面时
    // 用的地址，族由槽保证一致）。显式值必须是**具体**地址且与本族同族。
    std::string advertised_udp_ipv4;
    std::string advertised_udp_ipv6;
    // 通告端口覆盖（默认 = udp_port）。
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
    [[nodiscard]] std::uint64_t udp_heartbeat_received() const noexcept { return udp_.heartbeat_received(); }
    [[nodiscard]] std::uint64_t udp_heartbeat_rejected() const noexcept { return udp_.heartbeat_rejected(); }
    [[nodiscard]] std::uint64_t udp_sessions_established() const noexcept { return udp_.sessions_established(); }
    [[nodiscard]] std::uint64_t udp_sessions_refreshed() const noexcept { return udp_.sessions_refreshed(); }
    [[nodiscard]] std::uint64_t udp_heartbeat_ack_attempts() const noexcept { return udp_.heartbeat_ack_attempts(); }
    [[nodiscard]] std::uint64_t udp_malformed_datagrams() const noexcept { return udp_.malformed_datagrams(); }
    [[nodiscard]] std::uint64_t udp_non_heartbeat_datagrams() const noexcept { return udp_.non_heartbeat_datagrams(); }
    [[nodiscard]] net::UdpTransportStats udp_stats() const noexcept
    {
        return udp_.stats();
    }
    // 数据面**绑定成功**的族（语义与 `UdpServer::served_families()` 一致：`bind()` 是唯一
    // 写入者、`stop()` 不清除）。运行期读它等价于"正在服务的族"；但**停机后不要**拿它当
    // "当前还在服务"用。未服务的族在这里为 false，对应的通告槽也会是空的（客户端据此硬失败，
    // 服务端在 Connect 期直接拒绝）。
    [[nodiscard]] net::FamilySet udp_served_families() const noexcept { return udp_.served_families(); }
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
    void on_capture_event(const audio::AudioStreamEvent& event) noexcept;
    static void schedule_reap(const std::shared_ptr<ReapState>& reap,
        const std::weak_ptr<ServerRuntime>& weak_self,
        std::chrono::milliseconds interval, std::chrono::milliseconds timeout);
    void check_udp_receive_health(ReapState& reap, std::chrono::milliseconds interval) const;
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
    std::uint32_t effective_audio_queue_capacity_slots_ = 0;
    // RTP 发送流身份（每 run 一组，构造期随机）：ssrc 供 client 钉住流身份；
    // timestamp_offset 为首帧时间戳随机偏移（RFC 3550）。dispatcher 在
    // start() 前经 set_rtp_params 一次性接走（线程创建即屏障）。
    std::uint32_t rtp_ssrc_ = 0;
    std::uint32_t rtp_timestamp_offset_ = 0;
    std::shared_ptr<session::SessionManager> sessions_;
    net::UdpServer udp_;
    audio::AudioPacketizer packetizer_;
    audio::AudioFrameQueue frame_queue_;
    AudioNetworkDispatcher dispatcher_;
    std::unique_ptr<grpc::GrpcServer> grpc_;
    std::jthread grpc_thread_;
    struct ReapState {
        using Strand = asio::strand<asio::io_context::executor_type>;
        explicit ReapState(asio::io_context& ioc)
            : strand(asio::make_strand(ioc))
            , timer(std::make_shared<asio::steady_timer>(strand))
        {
        }
        Strand strand;
        std::shared_ptr<asio::steady_timer> timer;
        // UDP 收包健康观测（仅 reap strand 访问，无需原子）：**逐族**记录心跳计数与
        // 静默拍数。合计口径做不到"v4 在跳、v6 死了"这件事——而那正是双栈下最可能的
        // 故障形状（客户端族不被服务 / 该族被防火墙挡住），所以闸门必须是逐族的（INV-6）。
        // index: 0 = v4, 1 = v6, 2 = 族未定的 pending session 桶（Connect 期 peer 族
        // 解析失败的 fail-open 路径；只有这个桶能覆盖这种 session）。
        std::array<std::uint64_t, 3> last_heartbeat { };
        std::array<std::uint32_t, 3> silent_ticks { };
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

// ---- 控制面可达族 vs 数据面服务族（doc/network_design.md §3.3 / §7 INV-7）----
//
// INV-7 的后半句"控制面可达族 ⊆ 数据面服务族"**只在 `--rpc-ip` 通配时由构造成立**：
// gRPC 对通配绑定自带双栈（实测 0.0.0.0 与 :: 都产生 v4+v6 两个 TCP 监听），可达族即
// 两族，"槽的存在性 = 该族是否被服务"就够用了。pin 到具体地址后可达族收窄为那个地址
// 的族，它可能根本没被服务 —— 此时 server 会照常启动、却拒绝掉每一个客户端（或反过来
// 绑了一个谁也连不上的空转 socket）。这一层因此必须显式校验：CLI 不是公共入口，
// 只有 Core 自己守住才覆盖全部调用方。
struct ControlPlaneCoverage {
    // 能连上控制面的族（由 `--rpc-ip` 决定）。
    net::FamilySet reachable { };
    // 可达但没有被服务 ⇒ 该族客户端连得上控制面，却在 Connect 期被拒（白连一次）。
    net::FamilySet unserved { };
    // 被服务但不可达 ⇒ 该族 socket 绑上了，却没有任何客户端能连上它（空转）。
    net::FamilySet idle { };

    // 可达族一个都没被服务 ⇒ server 起来也没有意义，必须拒绝启动。
    // 可达族为空（地址解析失败）**不算**致命：与 Connect 期族准入的 fail-open 同一条
    // 约定 —— 判不出来时不误拒。
    [[nodiscard]] bool fatal() const noexcept
    {
        return !reachable.empty() && unserved.v4 == reachable.v4 && unserved.v6 == reachable.v6;
    }
};

// 控制面可达族：通配（`0.0.0.0` / `::`）⇒ 两族；具体地址 ⇒ 该地址所属的族。
// 地址解析失败 ⇒ 空集（调用方应已校验过字面量；空集不参与 fatal 判定，见上）。
[[nodiscard]] net::FamilySet control_plane_reachable_families(const std::string& rpc_ip);

// 可达族与服务族的覆盖判定。served 有两种用法：启动期先用**请求的**服务族做快速拒绝
// （发生在 bind 之前，不碰任何 socket），bind 之后再用**实际绑定结果**复核并 warn/debug
// ——请求了但没绑上的族，实际也服务不了。
[[nodiscard]] ControlPlaneCoverage check_control_plane_coverage(const std::string& rpc_ip,
    net::FamilySet served);

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
