#ifndef AQUA_GRPC_CLIENT_H
#define AQUA_GRPC_CLIENT_H

// gRPC 客户端：同步调用 Connect / Disconnect。
// 保活由 UDP HELLO 负责（刷新 NAT 映射 + server session last_seen），
// gRPC 不参与保活。
//
// 典型用法（client 侧）：
//   GrpcClient grpc;
//   ConnectResult res;
//   if (!grpc.connect_to_server(server_ip, rpc_port) || !grpc.connect(name, res)) {
//       return;
//   }
//   // res.session_id / res.udp_address / res.udp_port 交给 UdpClient 建立数据面。

#include "core/audio/audio_format.h"
#include "core/net/grpc/grpc_include.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aqua::grpc {

// Connect RPC 的结果：后续建立 UDP 数据面所需的全部信息。
struct ConnectResult {
    std::uint32_t session_id = 0;    // 服务端分配的 session ID（0 表示无效）
    std::string udp_address;         // UDP 数据面地址（服务端通告）
    std::uint16_t udp_port = 0;      // UDP 数据面端口（0 表示无效）
    audio::AudioFormat audio_format; // 服务端固定音频格式（编码/声道/采样率）
};

// gRPC 客户端：同步调用 Connect / Disconnect。
// 保活由 UDP HELLO 负责（刷新 NAT 映射 + server session last_seen），gRPC 不参与保活。
// 非线程安全：connect_to_server / connect / disconnect 应在同一调用线程按序使用。
class GrpcClient {
public:
    GrpcClient() = default;

    // 创建 channel 并等待 TCP 连接就绪（阻塞，超时 GRPC_CONNECT_DEADLINE）。
    // 失败返回 false；成功后 stub_ 可用，可随后多次调用 connect()。
    [[nodiscard]] bool connect_to_server(const std::string& server_ip, std::uint16_t rpc_port);

    // 调用 Connect RPC（阻塞，超时 GRPC_CONNECT_DEADLINE）。
    // 成功时填充 out（session_id / UDP endpoint / 音频格式）；失败返回 false。
    [[nodiscard]] bool connect(const std::string& client_name, ConnectResult& out);

    // 调用 Disconnect RPC（阻塞，超时 GRPC_DISCONNECT_DEADLINE）。
    // server 无此 session 或超时都返回 false（best-effort 清理，不阻塞 client 退出）。
    [[nodiscard]] bool disconnect(std::uint32_t session_id);

private:
    // 已连接 server 的 stub；未 connect_to_server 时为 null，此时调用
    // connect()/disconnect() 返回 false。
    std::unique_ptr<pb::AudioService::Stub> stub_;
};

} // namespace aqua::grpc

#endif // AQUA_GRPC_CLIENT_H
