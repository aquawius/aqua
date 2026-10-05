#include "aqua/net/grpc/grpc_server.h"
#include "aqua/audio/audio_format_converter.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/address/ip_family.h"
#include "aqua/net/grpc/grpc_config.h"

#include <chrono>
#include <format>
#include <grpc/grpc.h> // channel 参数宏（GRPC_ARG_*KEEPALIVE*/HTTP2_*）
#include <grpc/impl/channel_arg_names.h> // HTTP2 ping 策略宏的权威出处

namespace aqua::grpc {
namespace {

    // 槽的日志呈现：空槽（该族不服务）与 sentinel（通配地址）必须一眼可辨。
    std::string describe_slot(const std::optional<AdvertisedUdpEndpoint>& slot)
    {
        if (!slot) {
            return "<not served>";
        }
        const auto endpoint = ::aqua::net::format_host_port(slot->address, slot->port);
        try {
            if (::aqua::net::parse_ip_address(slot->address).is_unspecified()) {
                return endpoint + " (sentinel)";
            }
        } catch (const std::exception&) {
            // 配置期已校验；这里只是日志呈现，解析失败不改变行为。
        }
        return endpoint;
    }

} // namespace

// 构造：仅保存 session 引用与通告参数，不创建任何 session；
// session 的实际创建发生在 Connect RPC 调用时。
GrpcServerService::GrpcServerService(session::SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frame_count, UdpPlaneDescriptor udp_plane)
    : session_manager_(sessions)
    , server_format_(server_format)
    , frame_count_(frame_count)
    , udp_plane_(std::move(udp_plane))
{
    log_debug_fmt("GrpcServerService configured: format={}ch/{}Hz/enc={} frame_count={} udp_plane(v4={} v6={})",
        server_format_.channels, server_format_.sample_rate,
        static_cast<int>(server_format_.encoding), frame_count_,
        describe_slot(udp_plane_.udp), describe_slot(udp_plane_.udp6));
}

// Connect RPC：创建新 session，并把连接所需信息（session_id / 逐族 UDP 端点 /
// 固定 AudioFormat）回给客户端。
// 注意：本 RPC 不建立任何 UDP 状态——客户端需随后用 session_id 发 heartbeat 建连
// 完成握手（见 SessionManager::on_heartbeat），server 才记录其 NAT 地址。
//
// **族准入检查在任何状态变更之前**（`create_session()` 之前）：能通过控制面连上的
// 客户端，其族必须在数据面被服务，否则客户端会"连上但永远收不到 ACK"。放在
// create_session 之前是为了不需要回滚、也不留下垃圾 session。
::grpc::Status GrpcServerService::Connect(::grpc::ServerContext* ctx,
    const pb::ConnectRequest* req,
    pb::ConnectResponse* resp)
{
    if (req->client_name().empty() || req->client_name().size() > aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) {
        log_warn_fmt("gRPC Connect rejected invalid client_name length={}", req->client_name().size());
        return { ::grpc::StatusCode::INVALID_ARGUMENT,
            std::format("client_name must be 1..{} bytes", aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) };
    }

    // peer 是控制面的对端地址：它只在这里用来判定"客户端的族"——一个已经付过代价
    // 的观测（客户端能连上控制面，就说明该族可用）。ctx 理论非空，防御性判空；
    // 解析失败（unix: / 非 IP 形态 / 空串）一律视为**未知**，不得据此拒绝。
    const std::string peer = ctx ? ctx->peer() : std::string { };
    net::IpFamily peer_family = net::IpFamily::Unknown;
    if (const auto parsed = net::parse_grpc_peer_address(peer)) {
        peer_family = parsed->family;
    }
    log_debug_fmt("gRPC Connect: client_name='{}' peer='{}' peer_family={}",
        req->client_name(), ctx ? peer : std::string { "?" }, net::ip_family_name(peer_family));

    // 族准入（doc/network_design.md §3.3）：该族数据面未服务 ⇒ 立即拒绝（把"3 秒后客户端报 no heartbeat ACK、
    // 运维去查防火墙"换成连接期双方指名道姓的报错）。判不出族时放过。
    if (peer_family != net::IpFamily::Unknown && !udp_plane_.serves(peer_family)) {
        log_warn_fmt("gRPC Connect rejected: client peer_family={} but server serves only {}",
            net::ip_family_name(peer_family), udp_plane_.served().to_string());
        return { ::grpc::StatusCode::FAILED_PRECONDITION,
            std::format("this server does not serve the {} data plane (serving: {}); "
                        "connect over a family the server serves",
                peer_family == net::IpFamily::V4 ? "IPv4" : "IPv6",
                udp_plane_.served().to_string()) };
    }

    // 把控制面观测到的族记进 session：收包健康检查据此回答"这一族有没有人在等首包"
    // （未建连的 session 真正的族还是 Unknown，只有这里知道它期望哪一族）。
    auto id = session_manager_.create_session(peer_family);
    if (!id) {
        // 仅在 session ID 空间耗尽时发生（见 SessionManager::create_session）。
        log_error("Connect: failed to create session");
        return { ::grpc::StatusCode::INTERNAL, "session creation failed" };
    }

    // 回包：session_id + 逐族 UDP 端点（两个槽）+ 固定音频格式。
    // 槽存在性就是"该族是否被服务"的编码：未服务的族**不写**字段（has_udp6()==false）。
    // Connect 的 session 创建与响应构造视为一个事务：若 protobuf/分配操作抛异常，
    // 必须回滚刚创建的 session，避免 client 永远拿不到 session_id 却让 server 留下残留。
    try {
        resp->set_session_id(*id);
        if (udp_plane_.udp) {
            auto* slot = resp->mutable_udp();
            slot->set_address(udp_plane_.udp->address);
            slot->set_port(udp_plane_.udp->port);
        }
        if (udp_plane_.udp6) {
            auto* slot = resp->mutable_udp6();
            slot->set_address(udp_plane_.udp6->address);
            slot->set_port(udp_plane_.udp6->port);
        }
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

    log_debug_fmt("gRPC Connect response: session=0x{:08X} client_name='{}' udp(v4)={} udp6(v6)={}",
        *id, req->client_name(), describe_slot(udp_plane_.udp), describe_slot(udp_plane_.udp6));
    return ::grpc::Status::OK;
}

// Disconnect RPC：删除 session（幂等）。
// 客户端断开/崩溃后的残留 session 由存活超时清理兜底
// （SessionManager::remove_expired_sessions，超时基准是 proto Keepalive
//  刷新的 last_seen；UDP heartbeat 只更新 endpoint，不续存活），
// 本 RPC 只是主动删除的快捷路径。
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

// Keepalive：应用层探活。session 存在即刷新存活并返回 valid=true；
// 不存在返回 valid=false（client 应停止，而非重试——会话已经没了）。
// 无状态变更、无等待、无线程交互，任何工作线程可调。
::grpc::Status GrpcServerService::Keepalive(::grpc::ServerContext* /*ctx*/,
    const pb::KeepaliveRequest* req,
    pb::KeepaliveResponse* resp)
{
    const auto id = req->session_id();
    const bool valid = session_manager_.touch_session_liveness(id);
    resp->set_session_valid(valid);
    if (!valid) {
        log_debug_fmt("gRPC Keepalive: unknown session 0x{:08X}", id);
    }
    return ::grpc::Status::OK;
}

// ---- GrpcServer ----

// 构造即创建 service 并 BuildAndStart（非阻塞）：
//   - 绑定 bind_ip:rpc_port 提供 gRPC 服务；
//   - udp_plane 仅是通告数据，不在本类绑定 UDP（数据面绑定在 UdpServer）。
// 启动失败（端口被占用等）时 server_ 为空，is_running() 返回 false。
GrpcServer::GrpcServer(session::SessionManager& sessions, audio::AudioFormat server_format,
    std::uint32_t frame_count, std::string bind_ip, std::uint16_t rpc_port,
    UdpPlaneDescriptor udp_plane)
{
    std::string address;
    try {
        (void)::aqua::net::parse_ip_address(bind_ip); // 校验 IP 字面量（非 IP 抛异常）
        address = ::aqua::net::format_host_port(bind_ip, rpc_port);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC server rejected invalid bind address {} - {}", bind_ip, format_exception_message(e));
        return;
    }
    log_debug_fmt("GrpcServer configured: bind={} udp(v4)={} udp6(v6)={} format={}ch/{}Hz/enc={} frame_count={}",
        address, describe_slot(udp_plane.udp), describe_slot(udp_plane.udp6),
        server_format.channels, server_format.sample_rate,
        static_cast<int>(server_format.encoding), frame_count);

    service_ = std::make_unique<GrpcServerService>(
        sessions, server_format, frame_count, std::move(udp_plane));

    ::grpc::ServerBuilder builder;
    // 明文传输：仅在可信内网部署时使用；公网场景需换用 TLS 凭证。
    // 注意：这里故意不调任何 keepalive/HTTP2 channel 参数——存活探测走
    // proto Keepalive RPC（应用层行为，与 gRPC 版本无关）；传输层全部默认，
    // 从根上杜绝 GOAWAY 误杀那类版本相关调参事故。
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
// 注意：Shutdown() 会阻塞到所有在途 RPC handler 返回才完成——Keepalive 是
// 短平快的一元 RPC，不存在长连接 handler，因此不会自死锁。
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
