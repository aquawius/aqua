#include "aqua/net/grpc/grpc_server.h"
#include "aqua/audio/audio_format_converter.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

namespace aqua::grpc {

// 构造：仅保存 session 引用与通告参数，不创建任何 session；
// session 的实际创建发生在 Connect RPC 调用时。
GrpcServerService::GrpcServerService(SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frames_per_slot, std::string resp_udp_address, std::uint16_t resp_udp_port)
    : session_manager_(sessions)
    , server_format_(server_format)
    , frames_per_slot_(frames_per_slot)
    , resp_udp_address_(std::move(resp_udp_address))
    , resp_udp_port_(resp_udp_port)
{
}

// Connect RPC：创建新 session，并把连接所需信息（session_id / UDP endpoint /
// 固定 AudioFormat）回给客户端。
// 注意：本 RPC 不建立任何 UDP 状态——客户端需随后用 session_id 发 UDP HELLO
// 完成握手（见 SessionManager::establish_session），server 才记录其 NAT 地址。
::grpc::Status GrpcServerService::Connect(::grpc::ServerContext* ctx,
    const pb::ConnectRequest* req,
    pb::ConnectResponse* resp)
{
    // peer 为对端 socket 地址，仅用于日志排障（ctx 理论非空，防御性判空）。
    log_debug_fmt("gRPC Connect: client_name='{}' peer='{}'",
        req->client_name(), ctx ? ctx->peer() : std::string { "?" });

    auto id = session_manager_.create_session();
    if (!id) {
        // 仅在 session ID 空间耗尽时发生（见 SessionManager::create_session）。
        log_error("Connect: failed to create session");
        return { ::grpc::StatusCode::INTERNAL, "session creation failed" };
    }

    // 回包：session_id + UDP 数据面 endpoint + 固定音频格式。
    resp->set_session_id(*id);
    resp->mutable_udp()->set_address(resp_udp_address_);
    resp->mutable_udp()->set_port(resp_udp_port_);
    *resp->mutable_audio_format() = audio::to_proto(server_format_);
    resp->set_frames_per_slot(frames_per_slot_);

    std::string reply_endpoint;
    try {
        reply_endpoint = ::aqua::net::format_host_port(resp_udp_address_, resp_udp_port_);
    } catch (const std::exception&) {
        // 理论上构造 GrpcServer 时上层应已验证通告地址；这里仅作日志兜底，
        // 不让 diagnostics 因格式化异常影响 Connect RPC。
        reply_endpoint = resp_udp_address_ + ":" + std::to_string(resp_udp_port_);
    }
    log_info_fmt("Connect: session 0x{:08X} created (client_name='{}' reply endpoint='{}')",
        *id, req->client_name(), reply_endpoint);
    return ::grpc::Status::OK;
}

// Disconnect RPC：删除 session（幂等）。
// 客户端断开/崩溃后的残留 session 由 UDP 超时清理兜底
// （SessionManager::remove_expired_sessions），本 RPC 只是主动删除的快捷路径。
::grpc::Status GrpcServerService::Disconnect(::grpc::ServerContext* /*ctx*/,
    const pb::DisconnectRequest* req,
    pb::Empty* /*resp*/)
{
    log_debug_fmt("gRPC Disconnect: session=0x{:08X}", req->session_id());

    if (session_manager_.remove_session(req->session_id())) {
        log_info_fmt("Disconnect: session 0x{:08X} removed", req->session_id());
    } else {
        // 已不存在（超时清理或重复 Disconnect）：仍返回 OK 保持幂等。
        log_warn_fmt("Disconnect: session 0x{:08X} not found", req->session_id());
    }
    return ::grpc::Status::OK;
}

// ---- GrpcServer ----

// 构造即创建 service 并 BuildAndStart（非阻塞）：
//   - 绑定 bind_ip:rpc_port 提供 gRPC 服务；
//   - resp_udp_address / resp_udp_port 仅是通告数据，不在本类绑定 UDP。
// 启动失败（端口被占用等）时 server_ 为空，is_running() 返回 false。
GrpcServer::GrpcServer(SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frames_per_slot, std::string bind_ip, std::uint16_t rpc_port,
    std::string resp_udp_address, std::uint16_t resp_udp_port)
{
    service_ = std::make_unique<GrpcServerService>(
        sessions, server_format, frames_per_slot, std::move(resp_udp_address), resp_udp_port);

    std::string address;
    try {
        address = ::aqua::net::format_host_port(bind_ip, rpc_port);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC server rejected invalid bind address {} - {}", bind_ip, e.what());
        return;
    }
    ::grpc::ServerBuilder builder;
    // 明文传输：仅在可信内网部署时使用；公网场景需换用 TLS 凭证。
    builder.AddListeningPort(address, ::grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    if (server_) {
        started_ = true;
        log_info_fmt("gRPC server listening on {}", address);
    } else {
        log_error_fmt("gRPC server failed to start on {}", address);
    }
}

// 阻塞运行：等待 shutdown() 触发 Wait() 返回。
// 应在独立线程调用；返回后 running_ 复位为 false。
void GrpcServer::run()
{
    if (!server_) {
        // 构造失败（BuildAndStart 未成功）：立即返回，上层通过 is_running() 感知。
        log_error_fmt("gRPC server run failed");
        return;
    }
    running_.store(true, std::memory_order_release);
    server_->Wait(); // 阻塞直到 shutdown()
    running_.store(false, std::memory_order_release);
}

// 通知退出：gRPC 允许任意线程调用 Shutdown()，它会停止接收新请求并使
// Wait() 返回。调用后 server 不可重启（需新建 GrpcServer 实例）。
void GrpcServer::shutdown()
{
    if (server_) {
        server_->Shutdown();
    }
}

// started_（构造期写入，之后只读）与 running_（run 线程写入）组合判断：
// 两者同时为真才表示服务正在正常服务中。
bool GrpcServer::is_running() const noexcept
{
    return started_ && running_.load(std::memory_order_acquire);
}

} // namespace aqua::grpc
