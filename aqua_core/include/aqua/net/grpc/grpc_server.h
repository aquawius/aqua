#ifndef AQUA_NET_GRPC_GRPC_SERVER_H
#define AQUA_NET_GRPC_GRPC_SERVER_H

// gRPC 服务端：管理 session 生命周期（Connect / Disconnect / Keepalive），并通告客户端
// 建立 UDP 数据面所需的地址端口。
//
// 职责边界：
//   - gRPC 负责创建/删除 session 与 proto Keepalive 存活探活，不参与媒体传输；
//   - 存活由 UDP heartbeat（NAT/endpoint 续命 + 路径探活）与 proto Keepalive
//     （session 存活）分层负责，见 SessionManager 与 protocol.md §5。
//
// 典型用法（server 侧）：
//   GrpcServer grpc(sessions, fmt, 480, "0.0.0.0", 50051, UdpPlaneDescriptor::both(...));
//   std::thread t([&grpc] { grpc.run(); }); // run() 阻塞
//   ...
//   grpc.shutdown();                        // 通知 run() 返回
//   t.join();
//
// **数据面通告按客户端族分槽**（doc/network_design.md §3）：`udp` 给 IPv4 客户端、`udp6` 给 IPv6
// 客户端。客户端按自己连控制面时用的族选槽，所以服务端不需要猜 peer 的族——
// "控制面可达族 ⊆ 数据面服务族"这条不变量由构造成立，无需校验去守。

#include "aqua/audio/audio_format.h"
#include "aqua/net/address/ip_family.h"
#include "aqua/session/session_manager.h"

#include <aqua_service.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace aqua::grpc {

// 通告给客户端**某一个族的** UDP 数据面 endpoint（对应 ConnectResponse.udp / .udp6）。
// address/port 天然成对，合并成一个值类型，避免与 gRPC 监听的 rpc_port 混用/传反。
//
// address 的三种取值：
//   - 具体地址        → 客户端拨它（服务端永远下发与本槽族相同的地址，见 network_design.md §4.3）；
//   - 通配 0.0.0.0/:: → **sentinel**："用你自己连控制面时用的地址"；
//   - 字段不存在      → 本 server 不服务该族（用 std::optional 的 nullopt 表达）。
struct AdvertisedUdpEndpoint {
    std::string address;
    std::uint16_t port = 0;
};

// 逐族数据面通告（对应 ConnectResponse 的两个槽）。
// 槽存在 ⟺ 该族数据面 socket 绑定成功 —— 这条等价关系（INV-7）是整个方案
// 不需要"猜族"也不需要"候选回退"的根据。
struct UdpPlaneDescriptor {
    // 给 IPv4 客户端的槽；nullopt = 不服务 IPv4 数据面（wire 上字段不存在）。
    std::optional<AdvertisedUdpEndpoint> udp;
    // 给 IPv6 客户端的槽；nullopt = 不服务 IPv6 数据面。
    std::optional<AdvertisedUdpEndpoint> udp6;

    [[nodiscard]] static UdpPlaneDescriptor v4_only(AdvertisedUdpEndpoint v4)
    {
        return { std::move(v4), std::nullopt };
    }
    [[nodiscard]] static UdpPlaneDescriptor v6_only(AdvertisedUdpEndpoint v6)
    {
        return { std::nullopt, std::move(v6) };
    }
    [[nodiscard]] static UdpPlaneDescriptor both(AdvertisedUdpEndpoint v4, AdvertisedUdpEndpoint v6)
    {
        return { std::move(v4), std::move(v6) };
    }

    // 该族是否有可用槽（= 本实例是否服务该族数据面）。
    [[nodiscard]] bool serves(net::IpFamily family) const noexcept
    {
        return slot(family) != nullptr;
    }
    // 取槽值；Unknown 或该族未服务时返回 nullptr。
    [[nodiscard]] const AdvertisedUdpEndpoint* slot(net::IpFamily family) const noexcept
    {
        if (family == net::IpFamily::V4) {
            return udp ? &*udp : nullptr;
        }
        if (family == net::IpFamily::V6) {
            return udp6 ? &*udp6 : nullptr;
        }
        return nullptr;
    }
    [[nodiscard]] net::FamilySet served() const noexcept
    {
        return net::FamilySet { udp.has_value(), udp6.has_value() };
    }
};

// gRPC 服务实现：处理 Connect / Disconnect / Keepalive RPC。
// UDP 存活由 heartbeat + proto Keepalive 分层负责，gRPC 不包办保活。
// 持有 SessionManager 引用（不拥有），Server 固定 AudioFormat，
// 所有 session 共享同一格式。
class GrpcServerService final : public pb::AudioService::Service {
public:
    // sessions: 被引用但不拥有的 session 表（生命周期由上层保证）；
    // server_format: 通告给所有客户端的固定 PCM 格式；
    // udp_plane: 逐族通告的数据面端点（槽存在 ⟺ 该族绑定成功），与本服务实际
    // 监听的 gRPC bind_ip/rpc_port 无关。
    GrpcServerService(session::SessionManager& sessions, audio::AudioFormat server_format,
        std::uint32_t frame_count, UdpPlaneDescriptor udp_plane);

    // Connect：创建 session，返回 session_id + 逐族 UDP 端点（两个槽）+ 固定 AudioFormat。
    // client_name 必须为 1..128 bytes；非法请求返回 INVALID_ARGUMENT。
    // **族不匹配的请求在任何状态变更之前就被拒绝**（FAILED_PRECONDITION）：
    // 能通过控制面连上的客户端，其族必须在数据面被服务（doc/network_design.md §3.3 / INV-7），
    // 否则客户端会"连上但永远收不到 ACK"。peer 族无法判定时**不**拒绝（宁可漏判，
    // 不可误拒；漏掉的由客户端侧的空槽硬失败兜住）。
    // 仅在 session 创建失败（ID 空间耗尽）时返回其它非 OK 状态。
    ::grpc::Status Connect(::grpc::ServerContext* ctx,
        const pb::ConnectRequest* req,
        pb::ConnectResponse* resp) override;

    // Disconnect：删除 session。session 不存在时仍返回 OK（幂等语义），仅记 warning。
    ::grpc::Status Disconnect(::grpc::ServerContext* ctx,
        const pb::DisconnectRequest* req,
        pb::Empty* resp) override;

    // Keepalive：应用层探活。session 存在即刷新存活（last_seen）并返回
    // session_valid=true；不存在返回 valid=false（client 应停止，而非重试）。
    // 无状态、无锁外写（SessionManager 内部串行），任何工作线程可调。
    ::grpc::Status Keepalive(::grpc::ServerContext* ctx,
        const pb::KeepaliveRequest* req,
        pb::KeepaliveResponse* resp) override;

private:
    session::SessionManager& session_manager_; // 引用（不拥有），生命周期由上层保证
    audio::AudioFormat server_format_; // 通告给所有客户端的固定格式
    std::uint32_t frame_count_ = 0; // 通告的每 AudioFrame sample frame 数（F）
    UdpPlaneDescriptor udp_plane_; // 逐族通告的数据面端点（槽存在 = 服务该族）
};

// gRPC Server 包装：管理 builder / shutdown 生命周期。
// 构造即 BuildAndStart（非阻塞）；run() 阻塞等待服务结束；shutdown() 通知退出。
class GrpcServer {
public:
    // bind_ip 是 gRPC 控制面的本地监听地址（CLI 为 --rpc-ip）。数据面绑定是逐族的
    // （--udp-ipv4 / --udp-ipv6），与本参数无关。
    // IPv6 监听地址会格式化为 [addr]:port。
    // udp_plane 仅是回传给客户端的逐族数据面地址端口（与 bind_ip/rpc_port 无关）。
    GrpcServer(session::SessionManager& sessions, audio::AudioFormat server_format,
        std::uint32_t frame_count, std::string bind_ip, std::uint16_t rpc_port,
        UdpPlaneDescriptor udp_plane);

    // 启动 gRPC server（阻塞，内部 Wait()），在单独线程中调用。
    void run();

    // 通知 shutdown（非阻塞，使阻塞中的 run() 返回）。
    void shutdown();

    // BuildAndStart 是否成功；不依赖 run() 线程是否已经进入 Wait()。
    [[nodiscard]] bool is_started() const noexcept { return started_; }

    // 是否仍有 run() 线程在 Wait() 中。
    [[nodiscard]] bool is_running() const noexcept;

private:
    // 声明顺序 = 析构逆序。gRPC 契约要求 service 比 server 长寿（server 析构时
    // 会反注册 service），因此 service_ 先声明（后析构），server_ 后声明（先析构）。
    std::unique_ptr<GrpcServerService> service_;
    std::unique_ptr<::grpc::Server> server_;
    bool started_ = false; // BuildAndStart 是否成功（构造期写入，之后只读）
    std::atomic<bool> running_ { false }; // run() 是否仍在阻塞
};

} // namespace aqua::grpc

#endif // AQUA_NET_GRPC_GRPC_SERVER_H
