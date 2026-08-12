#include "core/grpc/grpc_server.h"

#include "core/grpc/audio_format_converter.h"
#include "core/logger/logger.h"

namespace aqua::grpc {

AudioServiceImpl::AudioServiceImpl(SessionManager& sessions, AudioFormat server_format,
                                   std::string udp_address, std::uint16_t udp_port)
    : sessions_(sessions)
    , server_format_(server_format)
    , udp_address_(std::move(udp_address))
    , udp_port_(udp_port)
{
}

::grpc::Status AudioServiceImpl::Connect(::grpc::ServerContext* /*ctx*/,
                                         const pb::ConnectRequest* req,
                                         pb::ConnectResponse* resp)
{
    auto id = sessions_.create_session();
    if (!id) {
        log_error("Connect: failed to create session");
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "session creation failed");
    }

    resp->set_session_id(*id);
    resp->mutable_udp()->set_address(udp_address_);
    resp->mutable_udp()->set_port(udp_port_);
    *resp->mutable_audio_format() = to_proto(server_format_);

    log_info_fmt("Connect: session 0x{:08X} created (client_name='{}')",
                 *id, req->client_name());
    return ::grpc::Status::OK;
}

::grpc::Status AudioServiceImpl::KeepAlive(::grpc::ServerContext* /*ctx*/,
                                           const pb::KeepAliveRequest* req,
                                           pb::KeepAliveResponse* resp)
{
    bool ok = sessions_.touch_session(req->session_id());
    resp->set_success(ok);
    if (!ok) {
        log_warn_fmt("KeepAlive: session 0x{:08X} not found", req->session_id());
    }
    return ::grpc::Status::OK;
}

::grpc::Status AudioServiceImpl::Disconnect(::grpc::ServerContext* /*ctx*/,
                                            const pb::DisconnectRequest* req,
                                            pb::Empty* /*resp*/)
{
    bool ok = sessions_.remove_session(req->session_id());
    if (ok) {
        log_info_fmt("Disconnect: session 0x{:08X} removed", req->session_id());
    } else {
        log_warn_fmt("Disconnect: session 0x{:08X} not found", req->session_id());
    }
    return ::grpc::Status::OK;
}

// ---- GrpcServer ----

GrpcServer::GrpcServer(SessionManager& sessions, AudioFormat server_format,
                       std::string bind_ip, std::uint16_t rpc_port,
                       std::string udp_address, std::uint16_t udp_port)
{
    service_ = std::make_unique<AudioServiceImpl>(
        sessions, server_format, std::move(udp_address), udp_port);

    std::string address = bind_ip + ":" + std::to_string(rpc_port);
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort(address, ::grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    if (server_) {
        log_info_fmt("gRPC server listening on {}", address);
    } else {
        log_error_fmt("gRPC server failed to start on {}", address);
    }
}

void GrpcServer::run()
{
    if (server_) {
        server_->Wait();
    }
}

void GrpcServer::shutdown()
{
    if (server_) {
        server_->Shutdown();
    }
}

} // namespace aqua::grpc
