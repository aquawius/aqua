#ifndef AQUA_GRPC_SERVER_H
#define AQUA_GRPC_SERVER_H

// gRPC 服务端：管理 session 生命周期（Connect / Disconnect），并通告客户端
// 建立 UDP 数据面所需的地址端口。
//
// 职责边界：
//   - gRPC 只负责创建/删除 session，不参与保活；
//   - 保活由 UDP HELLO 负责（server 收到 HELLO 后 establish_session，
//     幂等刷新 NAT 映射 endpoint + last_seen），见 SessionManager。
//
// 典型用法（server 侧）：
//   GrpcServer grpc(sessions, fmt, 480, "0.0.0.0", 50051, {advertised_ip, 9999});
//   std::thread t([&grpc] { grpc.run(); }); // run() 阻塞
//   ...
//   grpc.shutdown();                        // 通知 run() 返回
//   t.join();

#include "aqua/audio/audio_format.h"
#include "aqua/net/grpc/grpc_include.h"
#include "aqua/session/session_manager.h"

#include <atomic>
#include <memory>
#include <string>

namespace aqua::grpc {

// 通告给客户端的 UDP 数据面 endpoint（对应 ConnectResponse.udp）。
// address/port 天然成对，合并成一个值类型，避免与 gRPC 监听的 rpc_port 混用/传反。
struct AdvertisedUdpEndpoint {
    std::string address;
    std::uint16_t port = 0;
};

// gRPC 服务实现：处理 Connect / Disconnect RPC。
// 保活由 UDP HELLO 负责（server 收到 HELLO 后 establish_session，
// 幂等刷新 endpoint + last_seen），gRPC 不参与保活。
// 持有 SessionManager 引用（不拥有），Server 固定 AudioFormat，
// 所有 session 共享同一格式。
class GrpcServerService final : public pb::AudioService::Service {
public:
    // sessions: 被引用但不拥有的 session 表（生命周期由上层保证）；
    // server_format: 通告给所有客户端的固定 PCM 格式；
    // advertised_udp: 仅通告给客户端的 UDP 数据面 endpoint，
    // 与本服务实际监听的 gRPC bind_ip/rpc_port 无关。
    GrpcServerService(session::SessionManager& sessions, audio::AudioFormat server_format,
        std::uint32_t frame_count, AdvertisedUdpEndpoint advertised_udp);

    // Connect：创建 session，返回 session_id + UDP endpoint + 固定 AudioFormat。
    // 仅在 session 创建失败（ID 空间耗尽）时返回非 OK 状态。
    ::grpc::Status Connect(::grpc::ServerContext* ctx,
        const pb::ConnectRequest* req,
        pb::ConnectResponse* resp) override;

    // Disconnect：删除 session。session 不存在时仍返回 OK（幂等语义），仅记 warning。
    ::grpc::Status Disconnect(::grpc::ServerContext* ctx,
        const pb::DisconnectRequest* req,
        pb::Empty* resp) override;

private:
    session::SessionManager& session_manager_; // 引用（不拥有），生命周期由上层保证
    audio::AudioFormat server_format_; // 通告给所有客户端的固定格式
    std::uint32_t frame_count_ = 0; // 通告的每 AudioFrame sample frame 数（F）
    AdvertisedUdpEndpoint advertised_udp_; // 通告的 UDP 数据面 endpoint（地址+端口成对）
};

// gRPC Server 包装：管理 builder / shutdown 生命周期。
// 构造即 BuildAndStart（非阻塞）；run() 阻塞等待服务结束；shutdown() 通知退出。
class GrpcServer {
public:
    // bind_ip 支持 IPv4/IPv6 字面量；IPv6 监听地址会格式化为 [addr]:port。
    // advertised_udp 仅是回传给客户端的数据面地址端口（与 bind_ip/rpc_port 无关）。
    GrpcServer(session::SessionManager& sessions, audio::AudioFormat server_format,
        std::uint32_t frame_count, std::string bind_ip, std::uint16_t rpc_port,
        AdvertisedUdpEndpoint advertised_udp);

    // 启动 gRPC server（阻塞，内部 Wait()），在单独线程中调用。
    void run();

    // 通知 shutdown（非阻塞，使阻塞中的 run() 返回）。
    void shutdown();

    // BuildAndStart 是否成功；不依赖 run() 线程是否已经进入 Wait()。
    [[nodiscard]] bool is_started() const noexcept { return started_; }

    // 是否仍有 run() 线程在 Wait() 中。
    [[nodiscard]] bool is_running() const noexcept;

private:
    // 声明顺序 = 析构逆序。gRPC 契约要求 service 比 server 长寿（server 析构时
    // 会反注册 service），因此 service_ 先声明（后析构），server_ 后声明（先析构）。
    std::unique_ptr<GrpcServerService> service_;
    std::unique_ptr<::grpc::Server> server_;
    bool started_ = false; // BuildAndStart 是否成功（构造期写入，之后只读）
    std::atomic<bool> running_ { false }; // run() 是否仍在阻塞
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_SERVER_H
