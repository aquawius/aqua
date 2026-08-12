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

    pb::ConnectRequest req;
    req.set_client_name(client_name);

    pb::ConnectResponse resp;
    ::grpc::ClientContext ctx;

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

bool GrpcClient::keep_alive(std::uint32_t session_id)
{
    if (!stub_) return false;

    pb::KeepAliveRequest req;
    req.set_session_id(session_id);

    pb::KeepAliveResponse resp;
    ::grpc::ClientContext ctx;

    auto status = stub_->KeepAlive(&ctx, req, &resp);
    if (!status.ok()) {
        log_warn_fmt("gRPC KeepAlive failed: {}", status.error_message());
        return false;
    }
    return resp.success();
}

bool GrpcClient::disconnect(std::uint32_t session_id)
{
    if (!stub_) return false;

    pb::DisconnectRequest req;
    req.set_session_id(session_id);

    pb::Empty resp;
    ::grpc::ClientContext ctx;

    auto status = stub_->Disconnect(&ctx, req, &resp);
    if (!status.ok()) {
        log_warn_fmt("gRPC Disconnect failed: {}", status.error_message());
        return false;
    }
    return true;
}

} // namespace aqua::grpc
