#ifndef AQUA_UDP_CLIENT_H
#define AQUA_UDP_CLIENT_H

// UDP 客户端数据面（协议层，对称于 grpc::GrpcClient 的组织方式）：
//   - set_remote() 指定 server 数据面 endpoint（内部自动打开临时端口 socket）；
//   - start_receive() 启动收包：内部 decode wire 帧，Heartbeat/HeartbeatAck 内部消化，
//     Audio datagram 校验 SSRC、展开 16-bit 序号后以 extended sequence + PCM span 回调上交；
//   - start_heartbeat() 启动存活定时器：association 建立前按 handshake 节奏发 heartbeat
//     等 ACK 建连；首个有效 ACK 后自动转 HEARTBEAT_INTERVAL 单向续命（NAT 维持）。
//
// 典型用法：
//   UdpClient udp(ioc);
//   udp.set_remote(server_ip, udp_port);       // 来自 gRPC ConnectResponse
//   udp.start_receive(expected_payload_bytes,
//       [&](std::uint64_t sequence, std::span<const std::byte> pcm) { consume(sequence, pcm); });
//   udp.start_heartbeat(session_id, 1s);       // 握手建连，之后自动转续命节奏
//
// wire 布局见 network_frame.h（RTP 12B 大端音频头 + 遗留小端 Heartbeat）。
// 上层（ClientRuntime）只负责 gRPC 控制面与 JitterBuffer 组装。

#include "aqua/compat/move_only_function.h"
#include "aqua/net/udp/udp_transport.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace aqua::net {

// UDP 客户端数据面：接收 Audio 帧 + heartbeat 建连/续命。
// 生命周期安全：收包 handler 与 heartbeat 定时器回调由 transport strand / io_context
// 持有，可能在对象析构后短暂存活；它们只捕获共享 State，不捕获 this，无 UAF。
class UdpClient {
public:
    // 解码后的 Audio datagram 回调（transport strand 上触发）。
    // PCM 是非拥有视图，仅回调内有效；net 层不构造 audio-domain 对象。
    // 回调类型经 compat 别名声明（MSVC = move_only_function；libc++ 回退 std::function）。
    using FrameHandler = compat::MoveOnlyFunction<
        void(std::uint64_t sequence, std::span<const std::byte> pcm)>;
    // Phase 0 自适应延迟改造：arrival 观测 tap（transport strand 上触发）。
    // 每个解码成功的 Audio 包（无论下游接受与否——观测的是网络本身）上报
    // wire 序号/时间戳/SSRC + 到达时钟（steady ns），供 audio 域 JitterEstimator
    // 更新。为空 = 不观测（默认；零开销）。必须在 start_receive() 之前设置。
    using ArrivalObserver = compat::MoveOnlyFunction<void(
        std::uint16_t sequence, std::uint32_t timestamp, std::uint32_t ssrc, std::int64_t arrival_ns)>;
    // 当 HeartbeatAck 连续 miss 达到阈值时，在 transport strand 上调用一次。
    // 该回调仅作通知；由属主/runtime 决定后续的生命周期状态。
    using LivenessHandler = compat::MoveOnlyFunction<void(std::uint32_t consecutive_misses)>;

    // 创建 client（仅创建 transport，不打开 socket；打开由 set_remote()/start_receive()
    // 自动完成）。
    explicit UdpClient(asio::io_context& ioc);
    // 到达观测 tap（见 ArrivalObserver）：启动前配置，进入运行期后不可修改。
    void set_arrival_observer(ArrivalObserver observer);
    // 析构时自动 stop()：取消 Heartbeat 定时器并关闭 socket（幂等）。
    ~UdpClient();

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // 设置 server 数据面 endpoint（字符串版，来自 gRPC ConnectResponse）。
    // 内部自动打开临时端口 socket，并按远端地址族选择 IPv4/IPv6。
    // 必须在 start_receive()/start_heartbeat() 之前调用；进入数据面运行期后不可修改。
    // 远端端口为 0、地址非法或运行期修改时返回 false。
    bool set_remote(const std::string& server_ip, std::uint16_t port);

    // 启动接收（one-shot）：必须先 set_remote()；内部 decode wire 帧，Heartbeat/HeartbeatAck 内部消化，Audio 帧以
    // (sequence, PCM span) 回调上交。expected_payload_bytes 用于严格验证 Audio
    // datagram 的 payload 尺寸，为 0 时拒绝。net 层不关心音频 domain 的 frame_count。
    // 未打开 socket 时自动 open()（临时端口）。
    bool start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame);

    // 启动存活定时器并立即发送首个 heartbeat（须已 set_remote；one-shot，重复调用忽略）。
    // session_id 来自 gRPC ConnectResponse；handshake_interval 为握手期节奏
    // （association 建立后自动转 HEARTBEAT_INTERVAL，无需上层干预）。
    // liveness 语义（两手准备）：握手期连续 HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD 个周期无 ACK、
    // 稳态连续 HEARTBEAT_ACK_MISS_THRESHOLD 个周期无 ACK，都触发 on_liveness_failure
    // （只触发一次；上层置 Degraded 并退出）。server 对每个合法 heartbeat 都回 ACK。
    // 若同步调度 one-shot 安装任务失败则返回 false。
    bool start_heartbeat(std::uint32_t session_id, std::chrono::milliseconds handshake_interval,
        LivenessHandler on_liveness_failure = { });

    // 停止收发、取消 Heartbeat 定时器并关闭 socket（幂等）。停止后不可复用。
    void stop() noexcept;

    // ---- 状态透传（诊断用）----

    [[nodiscard]] bool has_remote() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint remote_endpoint() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;
    [[nodiscard]] UdpTransportStats stats() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_ack_count() const noexcept;
    [[nodiscard]] std::uint32_t consecutive_heartbeat_ack_misses() const noexcept;
    [[nodiscard]] std::int64_t heartbeat_ack_age_ms() const noexcept;
    [[nodiscard]] bool heartbeat_failed() const noexcept;
    [[nodiscard]] std::uint64_t audio_frames_accepted() const noexcept;
    // 音频接收序列缺口统计（诊断）：收到的 Audio 帧之间出现的序列跳跃。
    // 语义严格是"看到了缺口"而非"网络丢包"——缺口也可能来自 server 端丢帧/
    // 队列溢出；乱序落后帧不计（UDP 有序，落后即迟到、由 JB 判语义）。
    // 首个帧只建基线不计数；每次 start_receive（新会话）重置。
    [[nodiscard]] std::uint64_t rx_audio_sequence_gap_events() const noexcept;
    [[nodiscard]] std::uint64_t rx_audio_sequence_missing_frames() const noexcept;
    [[nodiscard]] std::uint64_t malformed_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t unexpected_sender_datagrams() const noexcept;
    // 当前学到的 UDP peer endpoint（HeartbeatAck 实际来源）；尚未学到返回 nullopt。
    // 线程安全：内部加锁拷贝。
    [[nodiscard]] std::optional<asio::ip::udp::endpoint> learned_peer_endpoint() const noexcept;
    [[nodiscard]] std::uint64_t wrong_session_acks() const noexcept;
    [[nodiscard]] std::uint64_t audio_payload_mismatches() const noexcept;
    [[nodiscard]] std::uint64_t non_audio_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_handshake_send_attempts() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_ack_miss_events() const noexcept;

private:
    // 全部可变状态：transport + 帧回调 + Heartbeat 定时器。
    // 独立 shared_ptr 持有，供收包 handler 与定时器回调捕获保活。
    struct State {
        explicit State(asio::io_context& ioc);

        asio::io_context& ioc;
        using Strand = asio::strand<asio::io_context::executor_type>;
        Strand strand;
        std::shared_ptr<UdpTransport> transport;

        std::atomic<bool> receive_started { false };
        std::atomic<bool> heartbeat_started { false };

        // UDP endpoint discovery：首个携带正确 session_id 的 HeartbeatAck 学习实际对端
        // endpoint（IPv6 隐私扩展/多地址下，源地址可与 gRPC 通告地址不同），之后每次
        // 有效 ACK 刷新。Audio 只能来自当前 learned endpoint；握手完成前为空。
        // 由收包 handler（transport strand）写、由查询（可能其它线程）读，用互斥量保护
        // （读写均为短临界区）。
        std::optional<asio::ip::udp::endpoint> learned_endpoint;
        mutable std::mutex learned_mutex;

        // Phase 0 arrival 观测 tap（启动前配置，运行期只在 transport strand 读；
        // 与 set_remote 同模型：配置阶段调用方保证 happens-before）。
        ArrivalObserver arrival_observer;

        // heartbeat 定时器及其相关状态只在 strand 上访问。stop() 通过 post
        // 将取消动作送入同一串行执行域，不跨线程直接操作 timer。
        // 存活分层：heartbeat 首包建 association（首个有效 ACK 前）；建立后同一定时器
        // 自动转稳态节奏（HEARTBEAT_INTERVAL 发 heartbeat + ACK 跟踪：连续
        // HEARTBEAT_ACK_MISS_THRESHOLD 个周期无 ACK 即路径死亡，经
        // on_liveness_failure 上报）。UDP 与控制面双致命，任一死亡上层即退出。
        std::atomic<bool> associated { false };
        // client→server 方向末次发包时刻（Heartbeat/heartbeat 发送时更新；下游音频
        // 不更新——下行包维持不了上行 NAT 映射）。heartbeat tick 据此做
        // activity-aware 跳过。
        std::atomic<std::int64_t> last_tx_ms { 0 };
        std::unique_ptr<asio::steady_timer> heartbeat_timer;
        // ACK 接收回调运行在 transport strand，而 Heartbeat 定时器运行在本 state strand。
        // 因此这些字段必须是原子的，即便其余 Heartbeat 定时器状态是 strand 内封闭的。
        std::atomic<std::uint32_t> heartbeat_session_id { 0 };
        std::chrono::milliseconds handshake_interval { 0 };
        std::atomic<std::uint64_t> heartbeat_ack_generation { 0 };
        std::uint64_t heartbeat_ack_generation_seen = 0;
        bool liveness_failed = false;
        LivenessHandler on_liveness_failure;
        std::atomic<bool> heartbeat_stopped { false };
        std::atomic<bool> heartbeat_failed { false };
        std::atomic<std::uint64_t> heartbeat_ack_count { 0 };
        std::atomic<std::uint64_t> heartbeat_handshake_send_attempts { 0 };
        std::atomic<std::uint64_t> audio_frames_accepted { 0 };
        // RTP 流身份：首个音频包钉住 SSRC（与 learned_endpoint 同模型），
        // 之后不等即丢；SSRC == 0 永不接受（server 保证非零）。
        // wire 16-bit 序号按 RFC 3550 附录 A 展开成 u64 extended sequence
        // 再上交（JB 内部一律 u64，不感知回绕）。以下字段只在收包 handler
        // （transport strand 单写者）访问，诊断无跨线程读，用原子与既有
        // 计数器风格一致。
        std::atomic<bool> rtp_ssrc_valid { false };
        std::atomic<std::uint32_t> expected_rtp_ssrc { 0 };
        std::atomic<bool> rtp_seq_valid { false };
        std::atomic<std::uint64_t> last_rtp_ext_seq { 0 };
        // 音频序列缺口统计（收包 handler 单写者 + 诊断 relaxed 读）：
        // 首个帧建基线后，每次 seq > last+1 计一个 gap 事件与缺失帧数。
        std::atomic<bool> rx_audio_seq_valid { false };
        std::atomic<std::uint64_t> last_rx_audio_seq { 0 };
        std::atomic<std::uint64_t> rx_audio_gap_events { 0 };
        std::atomic<std::uint64_t> rx_audio_missing_frames { 0 };
        std::atomic<std::uint64_t> malformed_datagrams { 0 };
        std::atomic<std::uint64_t> unexpected_sender_datagrams { 0 };
        std::atomic<std::uint64_t> wrong_session_acks { 0 };
        std::atomic<std::uint64_t> audio_payload_mismatches { 0 };
        std::atomic<std::uint64_t> non_audio_datagrams { 0 };
        std::atomic<std::uint32_t> heartbeat_ack_misses { 0 };
        std::atomic<std::uint64_t> heartbeat_ack_miss_events { 0 };
        std::atomic<std::int64_t> last_heartbeat_ack_ms { 0 };
    };

    // 存活节拍调度（单一定时器，节奏按 phase 定）：未 association 按
    // handshake_interval 发 heartbeat 等 ACK；已 association 按
    // HEARTBEAT_INTERVAL 发 heartbeat 并做 ACK 跟踪（activity-aware 跳过）。
    static void schedule_beat(const std::shared_ptr<State>& state);

    // 单周期 ACK 记账（只在 strand 上调用）：本周期内有新 ACK 到达则清零，
    // 否则 miss+1；达到 threshold 且未锁存时置 liveness_failed 并调 handler
    // （只调一次）。握手期与稳态共用，threshold 按 phase 传。
    static void account_ack_miss(const std::shared_ptr<State>& state, std::uint32_t threshold);

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_UDP_CLIENT_H
