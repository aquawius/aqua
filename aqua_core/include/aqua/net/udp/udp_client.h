#ifndef AQUA_UDP_CLIENT_H
#define AQUA_UDP_CLIENT_H

// UDP 客户端数据面（协议层，对称于 grpc::GrpcClient 的组织方式）：
//   - set_remote() 指定 server 数据面 endpoint（内部自动打开临时端口 socket）；
//   - start_receive() 启动收包：内部 decode wire 帧，Hello/HelloAck 内部消化，
//     Audio datagram 以 sequence + PCM span 回调上交；
//   - start_hello() 立即发送首个 HELLO，之后周期发送（NAT 保活 + server session last_seen 刷新，
//     内部 steady_timer，无需上层自建定时器）。
//
// 典型用法：
//   UdpClient udp(ioc);
//   udp.set_remote(server_ip, udp_port);       // 来自 gRPC ConnectResponse
//   udp.start_receive(expected_payload_bytes,
//       [&](std::uint64_t sequence, std::span<const std::byte> pcm) { consume(sequence, pcm); });
//   udp.start_hello(session_id, 1s);           // 周期 HELLO 保活
//
// wire 布局见 network_frame.h。上层（ClientRuntime）只负责 gRPC 控制面与
// JitterBuffer 组装。

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

// UDP 客户端数据面：接收 Audio 帧 + 周期 HELLO 保活。
// 生命周期安全：收包 handler 与 HELLO 定时器回调由 transport strand / io_context
// 持有，可能在对象析构后短暂存活；它们只捕获共享 State，不捕获 this，无 UAF。
class UdpClient {
public:
    // 解码后的 Audio datagram 回调（transport strand 上触发）。
    // PCM 是非拥有视图，仅回调内有效；net 层不构造 audio-domain 对象。
    // 回调类型经 compat 别名声明（MSVC = move_only_function；libc++ 回退 std::function）。
    using FrameHandler = compat::MoveOnlyFunction<
        void(std::uint64_t sequence, std::span<const std::byte> pcm)>;
    // 当 HELLO_ACK 连续 miss 达到阈值时，在 transport strand 上调用一次。
    // 该回调仅作通知；由属主/runtime 决定后续的生命周期状态。
    using LivenessHandler = compat::MoveOnlyFunction<void(std::uint32_t consecutive_misses)>;

    // 创建 client（仅创建 transport，不打开 socket；打开由 set_remote()/start_receive()
    // 自动完成）。
    explicit UdpClient(asio::io_context& ioc);
    // 析构时自动 stop()：取消 HELLO 定时器并关闭 socket（幂等）。
    ~UdpClient();

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // 设置 server 数据面 endpoint（字符串版，来自 gRPC ConnectResponse）。
    // 内部自动打开临时端口 socket，并按远端地址族选择 IPv4/IPv6。
    // 必须在 start_receive()/start_hello() 之前调用；进入数据面运行期后不可修改。
    // 远端端口为 0、地址非法或运行期修改时返回 false。
    bool set_remote(const std::string& server_ip, std::uint16_t port);

    // 启动接收（one-shot）：必须先 set_remote()；内部 decode wire 帧，Hello/HelloAck 内部消化，Audio 帧以
    // (sequence, PCM span) 回调上交。expected_payload_bytes 用于严格验证 Audio
    // datagram 的 payload 尺寸，为 0 时拒绝。net 层不关心音频 domain 的 frame_count。
    // 未打开 socket 时自动 open()（临时端口）。
    bool start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame);

    // 周期发送 HELLO(session_id) 保活（须已 set_remote；one-shot，重复调用忽略）。
    // session_id 来自 gRPC ConnectResponse；interval 建议远小于 server 的
    // UDP session 超时（默认 1s / 5s）。
    // 若同步调度 one-shot HELLO 安装任务失败则返回 false。
    // 一旦接受，调度器会异步安装在 state strand 上；极罕见的延迟分配/编码失败
    // 会停止 HELLO，并通过诊断/日志上报。
    bool start_hello(std::uint32_t session_id, std::chrono::milliseconds interval,
        LivenessHandler on_liveness_failure = { });

    // 停止收发、取消 HELLO 定时器并关闭 socket（幂等）。停止后不可复用。
    void stop() noexcept;

    // ---- 状态透传（诊断用）----

    [[nodiscard]] bool has_remote() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint remote_endpoint() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;
    [[nodiscard]] UdpTransportStats stats() const noexcept;
    [[nodiscard]] std::uint64_t hello_ack_count() const noexcept;
    [[nodiscard]] std::uint32_t consecutive_hello_ack_misses() const noexcept;
    [[nodiscard]] std::int64_t hello_ack_age_ms() const noexcept;
    [[nodiscard]] bool hello_failed() const noexcept;
    [[nodiscard]] std::uint64_t audio_frames_accepted() const noexcept;
    [[nodiscard]] std::uint64_t malformed_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t unexpected_sender_datagrams() const noexcept;
    // 当前学到的 UDP peer endpoint（HELLO_ACK 实际来源）；尚未学到返回 nullopt。
    // 线程安全：内部加锁拷贝。
    [[nodiscard]] std::optional<asio::ip::udp::endpoint> learned_peer_endpoint() const noexcept;
    [[nodiscard]] std::uint64_t wrong_session_acks() const noexcept;
    [[nodiscard]] std::uint64_t audio_payload_mismatches() const noexcept;
    [[nodiscard]] std::uint64_t non_audio_datagrams() const noexcept;
    [[nodiscard]] std::uint64_t hello_send_attempts() const noexcept;
    [[nodiscard]] std::uint64_t hello_ack_miss_events() const noexcept;

private:
    // 全部可变状态：transport + 帧回调 + HELLO 定时器。
    // 独立 shared_ptr 持有，供收包 handler 与定时器回调捕获保活。
    struct State {
        explicit State(asio::io_context& ioc);

        asio::io_context& ioc;
        using Strand = asio::strand<asio::io_context::executor_type>;
        Strand strand;
        std::shared_ptr<UdpTransport> transport;

        std::atomic<bool> receive_started { false };
        std::atomic<bool> hello_started { false };

        // UDP endpoint discovery：首个携带正确 session_id 的 HELLO_ACK 学习实际对端
        // endpoint（IPv6 隐私扩展/多地址下，源地址可与 gRPC 通告地址不同），之后每次
        // 有效 ACK 刷新。Audio 只能来自当前 learned endpoint；握手完成前为空。
        // 由收包 handler（transport strand）写、由查询（可能其它线程）读，用互斥量保护
        // （读写均为短临界区）。
        std::optional<asio::ip::udp::endpoint> learned_endpoint;
        mutable std::mutex learned_mutex;

        // HELLO 保活定时器及其相关状态只在 strand 上访问。stop() 通过 post
        // 将取消动作送入同一串行执行域，不跨线程直接操作 timer。
        std::unique_ptr<asio::steady_timer> hello_timer;
        // ACK 接收回调运行在 transport strand，而 HELLO 定时器运行在本 state strand。
        // 因此这些字段必须是原子的，即便其余 HELLO 定时器状态是 strand 内封闭的。
        std::atomic<std::uint32_t> hello_session_id { 0 };
        std::chrono::milliseconds hello_interval { 0 };
        std::atomic<std::uint64_t> hello_ack_generation { 0 };
        std::uint64_t hello_ack_generation_seen = 0;
        std::uint32_t consecutive_hello_ack_misses = 0;
        bool liveness_failed = false;
        LivenessHandler on_liveness_failure;
        std::atomic<bool> hello_stopped { false };
        std::atomic<bool> hello_failed { false };
        std::atomic<std::uint64_t> hello_ack_count { 0 };
        std::atomic<std::uint64_t> hello_send_attempts { 0 };
        std::atomic<std::uint64_t> audio_frames_accepted { 0 };
        std::atomic<std::uint64_t> malformed_datagrams { 0 };
        std::atomic<std::uint64_t> unexpected_sender_datagrams { 0 };
        std::atomic<std::uint64_t> wrong_session_acks { 0 };
        std::atomic<std::uint64_t> audio_payload_mismatches { 0 };
        std::atomic<std::uint64_t> non_audio_datagrams { 0 };
        std::atomic<std::uint32_t> hello_ack_misses { 0 };
        std::atomic<std::uint64_t> hello_ack_miss_events { 0 };
        std::atomic<std::int64_t> last_hello_ack_ms { 0 };
    };

    // 周期调度 HELLO；每个 interval 先检查 ACK generation，再发送下一次 HELLO。
    static void schedule_hello(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_UDP_CLIENT_H
