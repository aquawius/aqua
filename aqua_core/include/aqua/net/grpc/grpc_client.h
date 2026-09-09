#ifndef AQUA_GRPC_CLIENT_H
#define AQUA_GRPC_CLIENT_H

// gRPC 客户端：同步调用 Connect / Disconnect，另有 server 事件订阅。
// UDP heartbeat 只维持 NAT 映射与 server session last_seen；session/控制面存活
// 由 gRPC keepalive（channel 参数）判定，UDP 路径失败不再致命。
//
// 典型用法（client 侧）：
//   GrpcClient grpc;
//   ConnectResult res;
//   if (!grpc.connect_to_server(server_ip, rpc_port) || !grpc.connect(name, res)) {
//       return;
//   }
//   // res.session_id / res.advertised_udp_address / res.advertised_udp_port 交给 UdpClient 建立数据面。

#include "aqua/audio/audio_format.h"
#include "aqua/compat/move_only_function.h"

#include <aqua_service.grpc.pb.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace aqua::grpc {

// Connect RPC 的结果：后续建立 UDP 数据面所需的全部信息。
struct ConnectResult {
    std::uint32_t session_id = 0; // 服务端分配的 session ID（0 表示无效）
    std::string advertised_udp_address; // 服务端通告的 UDP 数据面地址（最终可用地址；server 通告 wildcard 时已由 client fallback 到 server_ip）
    std::uint16_t advertised_udp_port = 0; // 服务端通告的 UDP 数据面端口（0 表示无效）
    audio::AudioFormat audio_format; // server 当前音频流格式（编码/声道/采样率）
    std::uint32_t frame_count = 0; // 每 AudioFrame 的 sample frame 数（JitterBuffer 预分配依据）

    [[nodiscard]] bool is_valid() const noexcept
    {
        return session_id != 0
            && !advertised_udp_address.empty()
            && advertised_udp_port != 0
            && audio_format.is_valid()
            && frame_count != 0;
    }
};

// gRPC 客户端：同步调用 Connect / Disconnect，另有 server 事件订阅。
// UDP heartbeat 只维持 NAT 映射与 server session last_seen；session/控制面存活
// 由 gRPC keepalive（channel 参数）判定，UDP 路径失败不再致命。
// 非线程安全：connect_to_server / connect / disconnect 应在同一调用线程按序使用；
// 唯一的例外是 ping 线程（start_keepalive 内部创建，只读 stub_）与 stop_keepalive()
// 的配合，见下。ClientRuntime 是 one-shot（无重连），stub_ 创建后不再变更。
class GrpcClient {
public:
    // proto keepalive 探活结果：首次非 Ok 即调 handler 一次（调用方应停止，
    // 而非重试——控制面已死或会话已不在，重试没有意义）。
    enum class KeepaliveStatus {
        TransportDead, // RPC 失败：TCP 断 / server 不可达 / 超时
        SessionGone, // RPC 成功但 session_valid=false：会话已被 server 清理
    };
    // 首次非 Ok 即调一次（之后线程退出，teardown 由调用方接管）。
    // 在内部 ping 线程触发，只做置标志/post，不得阻塞。
    using KeepaliveHandler = compat::MoveOnlyFunction<void(KeepaliveStatus status)>;
    GrpcClient() = default;
    // 析构前自动 stop_keepalive()（join ping 线程）。
    ~GrpcClient();

    // 创建 channel 并等待 TCP 连接就绪（阻塞，超时 GRPC_CONNECT_DEADLINE）。
    // 失败返回 false；成功后 stub_ 可用，可随后多次调用 connect()。
    [[nodiscard]] bool connect_to_server(const std::string& server_ip, std::uint16_t rpc_port);

    // 调用 Connect RPC（阻塞，超时 GRPC_CONNECT_DEADLINE）。
    // 成功时填充 out（session_id / UDP endpoint / 音频格式）。若 server 通告的 UDP address
    // 为 0.0.0.0 / ::，自动回退到 connect_to_server() 使用的 server_ip。
    [[nodiscard]] bool connect(const std::string& client_name, ConnectResult& out);

    // 调用 Disconnect RPC（阻塞，超时 GRPC_DISCONNECT_DEADLINE）。
    // server 无此 session 或超时都返回 false（best-effort 清理，不阻塞 client 退出）。
    [[nodiscard]] bool disconnect(std::uint32_t session_id);

    // 启动 proto keepalive 探活（Connect 成功后调用一次）：内部起 ping 线程，
    // 每 interval 一次带 deadline 的 Keepalive RPC；首次非 Ok 即调 handler 一次
    // 随后线程退出（teardown 由调用方接管，不自动重试）。
    // stop_keepalive() 取消并 join（幂等）；重复启动会先停掉旧的不再用的循环。
    void start_keepalive(std::uint32_t session_id, std::chrono::milliseconds interval,
        KeepaliveHandler on_failure);

    // 停止探活并 join ping 线程（幂等，noexcept）。析构自动调用。
    void stop_keepalive() noexcept;

private:
    // 已连接 server 的 stub；未 connect_to_server 时为 null，此时调用
    // connect()/disconnect() 返回 false。
    std::unique_ptr<pb::AudioService::Stub> stub_;
    // connect_to_server() 最后成功连接的具体 IP。Server 通告 wildcard UDP 地址
    // 时，以此作为 UDP endpoint fallback；这里不能使用 0.0.0.0 / ::。
    std::string server_ip_;
    // ping 线程与取消状态：stop_keepalive() 置停止标志 + TryCancel 在途 RPC + join；
    // ping 线程结束即释放 context，不与析构竞争（join 先行）。
    std::mutex keepalive_mutex_;
    std::shared_ptr<::grpc::ClientContext> keepalive_ctx_;
    std::thread keepalive_thread_;
    std::atomic<bool> keepalive_stopped_ { true };
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_CLIENT_H
