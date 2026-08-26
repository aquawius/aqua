#ifndef AQUA_UDP_CLIENT_H
#define AQUA_UDP_CLIENT_H

// UDP 客户端数据面（协议层，对称于 grpc::GrpcClient 的组织方式）：
//   - set_remote() 指定 server 数据面 endpoint（内部自动打开临时端口 socket）；
//   - start() 启动收包：内部 decode wire 帧，Hello/HelloAck 内部消化，
//     Audio 帧组装为 AudioFrame 后回调上交；
//   - start_hello() 周期发送 HELLO（NAT 保活 + server session last_seen 刷新，
//     内部 steady_timer，无需上层自建定时器）。
//
// 典型用法：
//   UdpClient udp(ioc);
//   udp.set_remote(server_ip, udp_port);       // 来自 gRPC ConnectResponse
//   udp.start(frames_per_slot, [&](const audio::AudioFrame& f) { jb.push(f); });
//   udp.start_hello(session_id, 1s);           // 周期 HELLO 保活
//
// wire 布局见 network_frame.h。上层（ClientRuntime）只负责 gRPC 控制面与
// JitterBuffer 组装。

#include "aqua/audio/audio_frame.h"
#include "aqua/net/udp/udp_transport.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace aqua::net {

// UDP 客户端数据面：接收 Audio 帧 + 周期 HELLO 保活。
// 生命周期安全：收包 handler 与 HELLO 定时器回调由 transport strand / io_context
// 持有，可能在对象析构后短暂存活；它们只捕获共享 State，不捕获 this，无 UAF。
class UdpClient {
public:
    // Audio 帧回调（transport strand 上触发，禁止阻塞；抛出会被捕获并记录）。
    // frame.data 为非拥有视图（指向 transport 接收缓冲），仅回调内有效，
    // 需要保活必须自行拷贝。
    using FrameHandler = std::move_only_function<void(const audio::AudioFrame&)>;

    // 创建 client（仅创建 transport，不打开 socket；打开由 set_remote()/start()
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
    // AudioFrame 回调上交。frames_per_slot 为 AudioFrame 的固定帧数 F
    //（来自控制面，用于组装 AudioFrame），为 0 时拒绝。
    // 未打开 socket 时自动 open()（临时端口）。
    bool start(std::uint32_t frames_per_slot, FrameHandler on_frame);

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
        std::shared_ptr<UdpTransport> transport;

        std::uint32_t frames_per_slot = 0; // F（start 时写入，仅 strand 上读）
        FrameHandler on_frame; // Audio 帧回调（仅 strand 上访问）

        // HELLO 保活定时器（start_hello 时创建，指针此后不可变；回调仅持有
        // State，无 this）。停止由 hello_stopped 原子标志完成——不在 stop()
        // 中销毁定时器，避免与 io 线程上的回调链竞争；定时器随 State 析构。
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
