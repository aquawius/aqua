#ifndef AQUA_GRPC_SERVER_H
#define AQUA_GRPC_SERVER_H

#include "core/public/audio_format.h"
#include "core/session/session_manager.h"

#include <aqua_service.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace aqua::grpc {

// gRPC 服务实现：处理 Connect / Disconnect。
// 保活由 UDP HELLO 负责（server 收到 HELLO 后 establish_udp → touch_session），gRPC 不参与保活。
// 持有 SessionManager 引用（不拥有），Server 固定 AudioFormat。
class AudioServiceImpl final : public pb::AudioService::Service {
public:
    AudioServiceImpl(SessionManager& sessions, AudioFormat server_format,
                     std::string udp_address, std::uint16_t udp_port);

    ::grpc::Status Connect(::grpc::ServerContext* ctx,
                           const pb::ConnectRequest* req,
                           pb::ConnectResponse* resp) override;

    ::grpc::Status Disconnect(::grpc::ServerContext* ctx,
                              const pb::DisconnectRequest* req,
                              pb::Empty* resp) override;

private:
    SessionManager& sessions_;
    AudioFormat server_format_;
    std::string udp_address_;
    std::uint16_t udp_port_;
};

// gRPC Server 包装：管理 builder / shutdown 生命周期。
class GrpcServer {
public:
    GrpcServer(SessionManager& sessions, AudioFormat server_format,
               std::string bind_ip, std::uint16_t rpc_port,
               std::string udp_address, std::uint16_t udp_port);

    // 启动 gRPC server（阻塞），在单独线程中调用。
    void run();

    // 通知 shutdown（非阻塞）。
    void shutdown();

private:
    std::unique_ptr<::grpc::Server> server_;
    std::unique_ptr<AudioServiceImpl> service_;
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_SERVER_H
