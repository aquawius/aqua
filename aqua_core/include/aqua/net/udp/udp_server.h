#ifndef AQUA_UDP_SERVER_H
#define AQUA_UDP_SERVER_H

// UDP 服务端数据面（协议层，对称于 grpc::GrpcServer 的组织方式）：
// 持有 SessionManager，收包循环内部处理 HELLO 握手（establish_session +
// 回 HelloAck），音频广播经 send_audio() 完成（内部 encode NetworkFrame）。
//
// 职责边界：
//   - 本类只做"协议 + 广播"，不负责 session 超时清理（策略由上层
//     ServerRuntime 的 reap 定时器驱动 SessionManager 完成）；
//   - server 数据面收包只认 HELLO（Audio / HelloAck / malformed 一律丢弃，
//     server 不更新音频包的 last_seen）。
//
// 典型用法：
//   auto sessions = std::make_shared<SessionManager>();
//   UdpServer udp(ioc, sessions);
//   udp.bind("0.0.0.0", 9999);
//   udp.start();                    // 收包循环：HELLO → establish + Ack
//   udp.send_audio(frame);          // encode + 广播给 Connected sessions

#include "aqua/audio/audio_frame.h"
#include "aqua/net/udp/udp_transport.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace aqua::net {

// UDP 服务端数据面：HELLO 握手 + 音频广播。
// 生命周期安全：收包 handler 由 transport strand 持有，可能在对象析构后
// 短暂存活，handler 只捕获共享 State（含 shared_ptr<SessionManager>），
// 不捕获 this，不存在 UAF。
class UdpServer {
public:
    // sessions：共享所有权。transport strand 上的收包 handler 会在本对象
    // 析构后短暂存活，shared_ptr 保证 SessionManager 不先于 handler 亡。
    UdpServer(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sessions);
    // 析构时自动 stop()（幂等）。
    ~UdpServer();

    UdpServer(const UdpServer&) = delete;
    UdpServer& operator=(const UdpServer&) = delete;

    // 绑定固定端口（bind_ip 支持 IPv4/IPv6 字面量，"0.0.0.0" 监听所有接口）。
    // 返回 false 表示绑定失败；同一 endpoint 重复 bind 幂等成功。
    // 详见 UdpTransport::bind。
    bool bind(const std::string& bind_ip, std::uint16_t port);

    // 启动收包循环（须先 bind 成功）。内部处理 HELLO，无包上交。
    bool start();

    // 停止收发并关闭 socket（幂等）。停止后不可复用，重启请新建实例。
    void stop() noexcept;

    // 把一个 AudioFrame 编码为 wire 帧并广播给所有 Connected session
    //（线程安全，可由实时采集线程调用；无 session 时仅计数不发送）。
    // 分配失败时静默丢弃该帧（实时线程不允许抛出）。
    void send_audio(const audio::AudioFrame& frame) noexcept;

    // 已编码的 AudioFrame 数（广播尝试数），与有无接收者无关。
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept;
    // 传输统计快照（含义见 UdpTransportStats）。
    [[nodiscard]] UdpTransportStats stats() const noexcept;
    // bind 成功后的本地 endpoint 快照（bind 端口=0 时查询 OS 实际分配端口）。
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept;

private:
    // 全部可变状态：transport + SessionManager + 广播计数。
    // 独立 shared_ptr 持有，供 strand 上的收包 handler 捕获保活。
    struct State {
        State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess);

        std::shared_ptr<UdpTransport> transport;
        std::shared_ptr<session::SessionManager> sessions;
        std::atomic<std::uint64_t> frames_encoded { 0 };
    };

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_UDP_SERVER_H
