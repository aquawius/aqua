#include "aqua/net/grpc/grpc_server.h"
#include "aqua/audio/audio_format_converter.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"

#include <format>

namespace aqua::grpc {

// 构造：仅保存 session 引用与通告参数，不创建任何 session；
// session 的实际创建发生在 Connect RPC 调用时。
GrpcServerService::GrpcServerService(session::SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frame_count, AdvertisedUdpEndpoint advertised_udp)
    : session_manager_(sessions)
    , server_format_(server_format)
    , frame_count_(frame_count)
    , advertised_udp_(std::move(advertised_udp))
{
    log_debug_fmt("GrpcServerService configured: format={}ch/{}Hz/enc={} frame_count={} udp={}",
        server_format_.channels, server_format_.sample_rate,
        static_cast<int>(server_format_.encoding), frame_count_,
        ::aqua::net::format_host_port(advertised_udp_.address, advertised_udp_.port));
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
    if (req->client_name().empty() || req->client_name().size() > aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) {
        log_warn_fmt("gRPC Connect rejected invalid client_name length={}", req->client_name().size());
        return { ::grpc::StatusCode::INVALID_ARGUMENT,
            std::format("client_name must be 1..{} bytes", aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) };
    }
    log_debug_fmt("gRPC Connect: client_name='{}' peer='{}'",
        req->client_name(), ctx ? ctx->peer() : std::string { "?" });

    auto id = session_manager_.create_session();
    if (!id) {
        // 仅在 session ID 空间耗尽时发生（见 SessionManager::create_session）。
        log_error("Connect: failed to create session");
        return { ::grpc::StatusCode::INTERNAL, "session creation failed" };
    }

    // 回包：session_id + UDP 数据面 endpoint + 固定音频格式。
    // Connect 的 session 创建与响应构造视为一个事务：若 protobuf/分配操作抛异常，
    // 必须回滚刚创建的 session，避免 client 永远拿不到 session_id 却让 server 留下残留。
    try {
        resp->set_session_id(*id);
        resp->mutable_udp()->set_address(advertised_udp_.address);
        resp->mutable_udp()->set_port(advertised_udp_.port);
        *resp->mutable_audio_format() = audio::to_proto(server_format_);
        resp->set_frame_count(frame_count_);
    } catch (const std::exception& e) {
        (void)session_manager_.remove_session(*id);
        log_error_fmt("Connect: failed to build response for session 0x{:08X}: {}", *id, format_exception_message(e));
        return { ::grpc::StatusCode::INTERNAL, "failed to build connect response" };
    } catch (...) {
        (void)session_manager_.remove_session(*id);
        log_error_fmt("Connect: failed to build response for session 0x{:08X}", *id);
        return { ::grpc::StatusCode::INTERNAL, "failed to build connect response" };
    }

    const std::string reply_endpoint = ::aqua::net::format_host_port(advertised_udp_.address, advertised_udp_.port);
    log_debug_fmt("gRPC Connect response: session=0x{:08X} client_name='{}' endpoint='{}'",
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
        log_debug_fmt("Disconnect: session 0x{:08X} not found (already removed)", req->session_id());
    }
    return ::grpc::Status::OK;
}

// ---- GrpcServer ----

// 构造即创建 service 并 BuildAndStart（非阻塞）：
//   - 绑定 bind_ip:rpc_port 提供 gRPC 服务；
//   - advertised_udp 仅是通告数据，不在本类绑定 UDP。
// 启动失败（端口被占用等）时 server_ 为空，is_running() 返回 false。
GrpcServer::GrpcServer(session::SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frame_count, std::string bind_ip, std::uint16_t rpc_port,
    AdvertisedUdpEndpoint advertised_udp)
{
    std::string address;
    try {
        (void)::aqua::net::parse_ip_address(bind_ip); // 校验 IP 字面量（非 IP 抛异常）
        address = ::aqua::net::format_host_port(bind_ip, rpc_port);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC server rejected invalid bind address {} - {}", bind_ip, format_exception_message(e));
        return;
    }
    log_debug_fmt("GrpcServer configured: bind={} advertised_udp={} format={}ch/{}Hz/enc={} frame_count={}",
        address, ::aqua::net::format_host_port(advertised_udp.address, advertised_udp.port),
        server_format.channels, server_format.sample_rate,
        static_cast<int>(server_format.encoding), frame_count);

    service_ = std::make_unique<GrpcServerService>(
        sessions, server_format, frame_count, std::move(advertised_udp));

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
    log_debug("gRPC server entering Wait()");
    server_->Wait(); // 阻塞直到 shutdown()
    running_.store(false, std::memory_order_release);
    log_debug("gRPC server Wait() returned");
}

// 通知退出：gRPC 允许任意线程调用 Shutdown()，它会停止接收新请求并使
// Wait() 返回。调用后 server 不可重启（需新建 GrpcServer 实例）。
void GrpcServer::shutdown()
{
    if (server_) {
        log_debug("gRPC server shutdown requested");
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
