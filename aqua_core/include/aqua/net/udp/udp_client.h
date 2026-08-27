#ifndef AQUA_UDP_CLIENT_H
#define AQUA_UDP_CLIENT_H

// UDP 客户端数据面（协议层，对称于 grpc::GrpcClient 的组织方式）：
//   - set_remote() 指定 server 数据面 endpoint（内部自动打开临时端口 socket）；
//   - start_receive() 启动收包：内部 decode wire 帧，Hello/HelloAck 内部消化，
//     Audio datagram 以 sequence + PCM span 回调上交；
//   - start_hello() 周期发送 HELLO（NAT 保活 + server session last_seen 刷新，
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

#include "aqua/net/udp/udp_transport.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    using FrameHandler = std::move_only_function<
        void(std::uint64_t sequence, std::span<const std::byte> pcm)>;

    // 创建 client（仅创建 transport，不打开 socket；打开由 set_remote()/start_receive()
    // 自动完成）。
    explicit UdpClient(asio::io_context& ioc);
    // 析构时自动 stop()：取消 HELLO 定时器并关闭 socket（幂等）。
    ~UdpClient();

    UdpClient(const UdpClient&) = delete;
    UdpClient& operator=(const UdpClient&) = delete;

    // 设置 server 数据面 endpoint（字符串版，来自 gRPC ConnectResponse）。
    // 内部自动打开临时端口 socket，并按远端地址族选择 IPv4/IPv6。
    // 远端端口为 0 或地址非法时返回 false。
    bool set_remote(const std::string& server_ip, std::uint16_t port);

    // 启动接收：内部 decode wire 帧，Hello/HelloAck 内部消化，Audio 帧以
    // (sequence, PCM span) 回调上交。expected_payload_bytes 用于严格验证 Audio
    // datagram 的 payload 尺寸，为 0 时拒绝。net 层不关心音频 domain 的 frame_count。
    // 未打开 socket 时自动 open()（临时端口）。
    bool start_receive(std::size_t expected_payload_bytes, FrameHandler on_frame);

    // 周期发送 HELLO(session_id) 保活（须已 set_remote；幂等：重复调用忽略）。
    // session_id 来自 gRPC ConnectResponse；interval 建议远小于 server 的
    // UDP session 超时（默认 1s / 5s）。
    void start_hello(std::uint32_t session_id, std::chrono::milliseconds interval);

    // 停止收发、取消 HELLO 定时器并关闭 socket（幂等）。停止后不可复用。
    void stop() noexcept;

    // ---- 状态透传（诊断用）----

    [[nodiscard]] bool has_remote() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint remote_endpoint() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;
    [[nodiscard]] UdpTransportStats stats() const noexcept;

private:
    // 全部可变状态：transport + 帧回调 + HELLO 定时器。
    // 独立 shared_ptr 持有，供收包 handler 与定时器回调捕获保活。
    struct State {
        explicit State(asio::io_context& ioc);

        asio::io_context& ioc;
        using Strand = asio::strand<asio::io_context::executor_type>;
        Strand strand;
        std::shared_ptr<UdpTransport> transport;

        std::size_t expected_payload_bytes = 0;
        FrameHandler on_frame; // Audio datagram 回调（仅 strand 上访问）

        // HELLO 保活定时器及其相关状态只在 strand 上访问。stop() 通过 post
        // 将取消动作送入同一串行执行域，不跨线程直接操作 timer。
        std::unique_ptr<asio::steady_timer> hello_timer;
        std::uint32_t hello_session_id = 0;
        std::chrono::milliseconds hello_interval { 0 };
        std::atomic<bool> hello_stopped { false };
    };

    // 周期调度 HELLO（回调链自续，直到 stop 取消定时器）。
    static void schedule_hello(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_UDP_CLIENT_H
