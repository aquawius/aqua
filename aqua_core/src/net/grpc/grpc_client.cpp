#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/grpc/grpc_config.h"

#include "aqua/audio/audio_format_converter.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <chrono>
#include <format>
#include <limits>
#include <string_view>

namespace aqua::grpc {

// 创建 channel 并阻塞等待 TCP 连接就绪。
// gRPC channel 的建立是异步的，不显式等待就直接发 RPC 会在首次调用时
// 阻塞较长时间（默认连接超时约 30s），这里主动等待并给出明确失败反馈。
bool GrpcClient::connect_to_server(const std::string& server_ip, std::uint16_t rpc_port)
{
    // A failed reconnect must not leave an older stub/server_ip pair usable by a later
    // connect() call. GrpcClient is normally one-shot, but making reconnect semantics
    // transactional keeps the fallback address tied to the current channel.
    stub_.reset();
    server_ip_.clear();

    if (rpc_port == 0) {
        log_error("gRPC: RPC port must not be zero");
        return false;
    }

    std::string target;
    try {
        target = net::format_host_port(server_ip, rpc_port);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC: invalid server address {} - {}", server_ip, format_exception_message(e));
        return false;
    }
    log_debug_fmt("gRPC: creating insecure channel target={} deadline={}ms", target,
        std::chrono::duration_cast<std::chrono::milliseconds>(config::GRPC_CONNECT_DEADLINE).count());
    auto channel = ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials());
    log_debug("gRPC: waiting for channel connectivity");

    // 等待连接就绪，超时 GRPC_CONNECT_DEADLINE 秒
    auto deadline = std::chrono::system_clock::now() + config::GRPC_CONNECT_DEADLINE;
    if (!channel->WaitForConnected(deadline)) {
        // GetState(false) 不尝试触发连接，只返回当前状态，用于日志展示。
        auto state = channel->GetState(false);
        log_error_fmt("gRPC: failed to connect to {} (state={})", target,
            static_cast<int>(state));
        return false;
    }

    server_ip_ = server_ip;
    stub_ = pb::AudioService::NewStub(channel);
    log_debug_fmt("gRPC: channel connected and stub initialized for {}", target);
    log_info_fmt("gRPC: connected to {}", target);
    return true;
}

// 调用 Connect RPC，成功后把建立 UDP 数据面所需的信息写入 out。
// 全程同步阻塞（含 deadline）；RPC 失败或返回数据非法时返回 false。
// 若 RPC 已创建 session 但响应校验失败，会 best-effort Disconnect 回滚，并保证 out 清空。
bool GrpcClient::connect(const std::string& client_name, ConnectResult& out)
{
    out = {};
    if (!stub_) {
        log_error("gRPC Connect rejected: client is not connected to a server");
        return false;
    }

    log_debug_fmt("gRPC Connect: calling RPC (client_name='{}')", client_name);

    pb::ConnectRequest req;
    req.set_client_name(client_name);

    pb::ConnectResponse resp;
    ::grpc::ClientContext ctx;

    // 设置 deadline：与 connect_to_server 的 WaitForConnected 使用同一常量
    // （GRPC_CONNECT_DEADLINE），避免 server TCP 已连接但 RPC 线程卡死时无限阻塞。
    ctx.set_deadline(std::chrono::system_clock::now() + config::GRPC_CONNECT_DEADLINE);

    auto status = stub_->Connect(&ctx, req, &resp);
    if (!status.ok()) {
        log_error_fmt("gRPC Connect failed: {} (code={})", status.error_message(),
            static_cast<int>(status.error_code()));
        return false;
    }

    const auto session_id = resp.session_id();
    log_debug_fmt("gRPC Connect RPC succeeded: session=0x{:08X} udp_address='{}' udp_port={} frame_count={}",
        session_id, resp.udp().address(), resp.udp().port(), resp.frame_count());
    const auto cleanup_failed_connect = [&](std::string_view reason) {
        log_error_fmt("gRPC Connect returned invalid data: {}", reason);
        if (session_id != 0) {
            try {
                (void)disconnect(session_id);
            } catch (const std::exception& e) {
                log_warn_fmt("gRPC Connect rollback Disconnect threw: {}", format_exception_message(e));
            } catch (...) {
                log_warn("gRPC Connect rollback Disconnect threw unknown exception");
            }
        }
        out = {};
        return false;
    };

    if (session_id == 0) {
        log_error("gRPC Connect returned invalid session_id 0");
        return false;
    }

    auto udp_address = resp.udp().address();
    bool fallback_to_server_ip = udp_address.empty();
    if (!fallback_to_server_ip) {
        try {
            fallback_to_server_ip = net::parse_ip_address(udp_address).is_unspecified();
        } catch (const std::exception& e) {
            log_warn_fmt("gRPC Connect: server returned invalid UDP address '{}'; falling back to gRPC server address {} ({})",
                udp_address, server_ip_, format_exception_message(e));
            fallback_to_server_ip = true;
        }
    }

    if (fallback_to_server_ip) {
        if (server_ip_.empty()) {
            return cleanup_failed_connect("UDP address is unusable and gRPC server address is unavailable for fallback");
        }
        log_debug_fmt("gRPC Connect: UDP address '{}' is unusable; falling back to gRPC server address {}",
            udp_address, server_ip_);
        udp_address = server_ip_;
        try {
            const auto fallback = net::parse_ip_address(udp_address);
            if (fallback.is_unspecified()) {
                return cleanup_failed_connect("gRPC server address is also unspecified; cannot resolve UDP endpoint");
            }
        } catch (const std::exception& e) {
            return cleanup_failed_connect(std::format("gRPC server address {} is invalid - {}", server_ip_, format_exception_message(e)));
        }
    }

    // proto 的 port 是 uint32，截断到 uint16 前必须校验范围，否则服务器返回的
    // 非法端口会被静默截断成错误端口（连到错误的 UDP 端点）。
    const std::uint32_t udp_port = resp.udp().port();
    if (udp_port == 0 || udp_port > std::numeric_limits<std::uint16_t>::max()) {
        return cleanup_failed_connect(std::format("invalid UDP port {}", udp_port));
    }

    const auto audio_format = audio::from_proto(resp.audio_format());
    if (!audio_format.is_valid()) {
        return cleanup_failed_connect("invalid audio format");
    }

    const auto frame_count = resp.frame_count();
    if (frame_count == 0) {
        return cleanup_failed_connect("invalid frame_count 0");
    }

    out.session_id = session_id;
    out.udp_address = udp_address;
    out.udp_port = static_cast<std::uint16_t>(udp_port);
    out.audio_format = audio_format;
    out.frame_count = frame_count;

    log_debug_fmt("gRPC Connect accepted: session=0x{:08X} udp={} format={}ch/{}Hz/enc={} frames_per_packet={}",
        out.session_id, net::format_host_port(out.udp_address, out.udp_port),
        out.audio_format.channels, out.audio_format.sample_rate,
        static_cast<int>(out.audio_format.encoding), out.frame_count);
    return true;
}

// 调用 Disconnect RPC（best-effort）。
// 设置短超时：server 可能已崩溃，同步等待会阻塞 ~2s（gRPC 默认重试）。
// GRPC_DISCONNECT_DEADLINE 足够局域网内完成 RPC；超时则放弃（不阻塞 client 退出）。
bool GrpcClient::disconnect(std::uint32_t session_id)
{
    if (!stub_ || session_id == 0) {
        log_trace_fmt("gRPC Disconnect skipped: stub={} session=0x{:08X}",
            stub_ ? "set" : "null", session_id);
        return false;
    }

    log_trace_fmt("gRPC Disconnect: calling RPC (session=0x{:08X})", session_id);

    pb::DisconnectRequest req;
    req.set_session_id(session_id);

    pb::Empty resp;
    ::grpc::ClientContext ctx;

    ctx.set_deadline(std::chrono::system_clock::now() + config::GRPC_DISCONNECT_DEADLINE);

    auto status = stub_->Disconnect(&ctx, req, &resp);
    if (!status.ok()) {
        log_warn_fmt("gRPC Disconnect failed: session=0x{:08X} code={} message={}",
            session_id, static_cast<int>(status.error_code()), status.error_message());
        return false;
    }
    log_trace_fmt("gRPC Disconnect OK: session=0x{:08X}", session_id);
    return true;
}

} // namespace aqua::grpc
