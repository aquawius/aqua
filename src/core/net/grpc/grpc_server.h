#ifndef AQUA_GRPC_SERVER_H
#define AQUA_GRPC_SERVER_H

#include "core/audio/audio_format.h"
#include "core/session/session_manager.h"

#include <aqua_service.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>

namespace aqua::grpc {

// gRPC 服务实现：处理 Connect / Disconnect.
// 保活由 UDP HELLO 负责(server 收到 HELLO 后 establish_udp, 幂等刷新 endpoint + last_seen),
// gRPC 不参与保活.
// 持有 SessionManager 引用(不拥有), Server 固定 AudioFormat.
class GrpcServerService final : public pb::AudioService::Service {
public:
    GrpcServerService(SessionManager& sessions, audio::AudioFormat server_format,
        std::string resp_udp_address, std::uint16_t resp_udp_port);

    ::grpc::Status Connect(::grpc::ServerContext* ctx,
        const pb::ConnectRequest* req,
        pb::ConnectResponse* resp) override;

    ::grpc::Status Disconnect(::grpc::ServerContext* ctx,
        const pb::DisconnectRequest* req,
        pb::Empty* resp) override;

private:
    SessionManager& session_manager_;
    audio::AudioFormat server_format_;
    std::string resp_udp_address_;
    std::uint16_t resp_udp_port;
};

// gRPC Server 包装：管理 builder / shutdown 生命周期.
class GrpcServer {
public:
    GrpcServer(SessionManager& sessions, audio::AudioFormat server_format,
        std::string bind_ip, std::uint16_t rpc_port,
        std::string resp_udp_address, std::uint16_t resp_udp_port);

    // 启动 gRPC server(会阻塞), 在单独线程中调用.
    void run();

    // 通知 shutdown(非阻塞, 通知阻塞的run()).
    void shutdown();

    // 是否成功启动并仍在运行(即 server_->Wait() 尚未返回).
    // 构造失败或 run() 已返回时返回 false.上层应在启动后检查此标志,
    // 失败时跳过后续资源启动并执行清理.
    bool is_running() const noexcept;

private:
    // 声明顺序 = 析构逆序.service_ 必须比 server_ 长寿(gRPC 契约),
    // 因此 service_ 先声明(后析构), server_ 后声明(先析构).
    std::unique_ptr<GrpcServerService> service_;
    std::unique_ptr<::grpc::Server> server_;
    bool started_ = false; // BuildAndStart 是否成功
    std::atomic<bool> running_ { false }; // run() 是否仍在阻塞
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_SERVER_H
