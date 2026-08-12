#ifndef AQUA_GRPC_CLIENT_H
#define AQUA_GRPC_CLIENT_H

#include "core/public/audio_format.h"

#include <aqua_service.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <string>

namespace aqua::grpc {

// Connect 返回的结果
struct ConnectResult {
    std::uint32_t session_id = 0;
    std::string udp_address;
    std::uint16_t udp_port = 0;
    AudioFormat audio_format;
};

// gRPC 客户端：同步调用 Connect / Disconnect。
// 保活由 UDP HELLO 负责（刷新 NAT 映射 + server session last_seen），gRPC 不参与保活。
class GrpcClient {
public:
    GrpcClient() = default;

    // 连接到 server gRPC 端口。返回 false 表示无法连接。
    bool connect_to_server(const std::string& server_ip, std::uint16_t rpc_port);

    // 调用 Connect RPC。返回 false 表示失败。
    bool connect(const std::string& client_name, ConnectResult& out);

    // 调用 Disconnect RPC。
    bool disconnect(std::uint32_t session_id);

private:
    std::unique_ptr<pb::AudioService::Stub> stub_;
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_CLIENT_H
