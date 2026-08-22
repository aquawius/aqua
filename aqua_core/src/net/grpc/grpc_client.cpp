#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/grpc/grpc_config.h"

#include "aqua/audio/audio_format_converter.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"

#include <chrono>

namespace aqua::grpc {

// 创建 channel 并阻塞等待 TCP 连接就绪。
// gRPC channel 的建立是异步的，不显式等待就直接发 RPC 会在首次调用时
// 阻塞较长时间（默认连接超时约 30s），这里主动等待并给出明确失败反馈。
bool GrpcClient::connect_to_server(const std::string& server_ip, std::uint16_t rpc_port)
{
    if (rpc_port == 0) {
        log_error("gRPC: RPC port must not be zero");
        return false;
    }

    std::string target;
    try {
        target = net::format_host_port(server_ip, rpc_port);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC: invalid server address {} - {}", server_ip, e.what());
        return false;
    }
    auto channel = ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials());

    // 等待连接就绪，超时 GRPC_CONNECT_DEADLINE 秒
    auto deadline = std::chrono::system_clock::now() + config::GRPC_CONNECT_DEADLINE;
    if (!channel->WaitForConnected(deadline)) {
        // GetState(false) 不尝试触发连接，只返回当前状态，用于日志展示。
        auto state = channel->GetState(false);
        log_error_fmt("gRPC: failed to connect to {} (state={})", target,
            static_cast<int>(state));
        return false;
    }

    stub_ = pb::AudioService::NewStub(channel);
    log_info_fmt("gRPC: connected to {}", target);
    return true;
}

// 调用 Connect RPC，成功后把建立 UDP 数据面所需的信息写入 out。
// 全程同步阻塞（含 deadline）；RPC 失败或返回数据非法时返回 false。
bool GrpcClient::connect(const std::string& client_name, ConnectResult& out)
{
    if (!stub_)
        return false;

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

    out.session_id = resp.session_id();
    if (out.session_id == 0) {
        log_error("gRPC Connect returned invalid session_id 0");
        return false;
    }

    out.udp_address = resp.udp().address();
    if (out.udp_address.empty()) {
        log_error("gRPC Connect returned empty UDP address");
        return false;
    }
    try {
        (void)net::parse_ip_address(out.udp_address);
    } catch (const std::exception& e) {
        log_error_fmt("gRPC Connect returned invalid UDP address {} - {}",
            out.udp_address, e.what());
        return false;
    }

    // proto 的 port 是 uint32，截断到 uint16 前必须校验范围，否则服务器返回的
    // 非法端口会被静默截断成错误端口（连到错误的 UDP 端点）。
    const std::uint32_t udp_port = resp.udp().port();
    if (udp_port == 0 || udp_port > 65535) {
        log_error_fmt("gRPC Connect returned invalid UDP port {}", udp_port);
        return false;
    }
    out.udp_port = static_cast<std::uint16_t>(udp_port);
    out.audio_format = audio::from_proto(resp.audio_format());
    if (!out.audio_format.is_valid()) {
        log_error("gRPC Connect returned invalid audio format");
        return false;
    }

    log_info_fmt("gRPC Connect OK: session=0x{:08X} udp={} format={}ch/{}Hz/enc={}",
        out.session_id, net::format_host_port(out.udp_address, out.udp_port),
        out.audio_format.channels, out.audio_format.sample_rate,
        static_cast<int>(out.audio_format.encoding));
    return true;
}

// 调用 Disconnect RPC（best-effort）。
// 设置短超时：server 可能已崩溃，同步等待会阻塞 ~2s（gRPC 默认重试）。
// GRPC_DISCONNECT_DEADLINE 足够局域网内完成 RPC；超时则放弃（不阻塞 client 退出）。
bool GrpcClient::disconnect(std::uint32_t session_id)
{
    if (!stub_ || session_id == 0)
        return false;

    log_debug_fmt("gRPC Disconnect: calling RPC (session=0x{:08X})", session_id);

    pb::DisconnectRequest req;
    req.set_session_id(session_id);

    pb::Empty resp;
    ::grpc::ClientContext ctx;

    ctx.set_deadline(std::chrono::system_clock::now() + config::GRPC_DISCONNECT_DEADLINE);

    auto status = stub_->Disconnect(&ctx, req, &resp);
    if (!status.ok()) {
        log_warn_fmt("gRPC Disconnect failed: {}", status.error_message());
        return false;
    }
    return true;
}

} // namespace aqua::grpc
