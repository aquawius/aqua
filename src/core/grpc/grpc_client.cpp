#include "core/grpc/grpc_client.h"

#include "core/grpc/audio_format_converter.h"
#include "core/logger/logger.h"

#include <chrono>

namespace aqua::grpc {

bool GrpcClient::connect_to_server(const std::string& server_ip, std::uint16_t rpc_port)
{
    std::string target = server_ip + ":" + std::to_string(rpc_port);
    auto channel = ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials());

    // 等待连接就绪，超时 5 秒
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    if (!channel->WaitForConnected(deadline)) {
        auto state = channel->GetState(false);
        log_error_fmt("gRPC: failed to connect to {} (state={})", target,
                      static_cast<int>(state));
        return false;
    }

    stub_ = pb::AudioService::NewStub(channel);
    log_info_fmt("gRPC: connected to {}", target);
    return true;
}

bool GrpcClient::connect(const std::string& client_name, ConnectResult& out)
{
    if (!stub_) return false;

    log_debug_fmt("gRPC Connect: calling RPC (client_name='{}')", client_name);

    pb::ConnectRequest req;
    req.set_client_name(client_name);

    pb::ConnectResponse resp;
    ::grpc::ClientContext ctx;

    // 设置 deadline：与 connect_to_server 的 5s WaitForConnected 对齐，
    // 避免 server TCP 已连接但 RPC 线程卡死时无限阻塞。
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    auto status = stub_->Connect(&ctx, req, &resp);
    if (!status.ok()) {
        log_error_fmt("gRPC Connect failed: {} (code={})", status.error_message(),
                      static_cast<int>(status.error_code()));
        return false;
    }

    out.session_id = resp.session_id();
    out.udp_address = resp.udp().address();
    out.udp_port = static_cast<std::uint16_t>(resp.udp().port());
    out.audio_format = from_proto(resp.audio_format());

    log_info_fmt("gRPC Connect OK: session=0x{:08X} udp={}:{} format={}ch/{}Hz/enc={}",
                 out.session_id, out.udp_address, out.udp_port,
                 out.audio_format.channels, out.audio_format.sample_rate,
                 static_cast<int>(out.audio_format.encoding));
    return true;
}

bool GrpcClient::disconnect(std::uint32_t session_id)
{
    if (!stub_) return false;

    log_debug_fmt("gRPC Disconnect: calling RPC (session=0x{:08X})", session_id);

    pb::DisconnectRequest req;
    req.set_session_id(session_id);

    pb::Empty resp;
    ::grpc::ClientContext ctx;

    // 设置短超时：server 可能已崩溃，同步等待会阻塞 ~2s（gRPC 默认重试）。
    // 500ms 足够局域网内完成 RPC；超时则放弃（best-effort，不阻塞 client 退出）。
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));

    auto status = stub_->Disconnect(&ctx, req, &resp);
    if (!status.ok()) {
        log_warn_fmt("gRPC Disconnect failed: {}", status.error_message());
        return false;
    }
    return true;
}

} // namespace aqua::grpc
