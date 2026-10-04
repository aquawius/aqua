#include "aqua/net/udp/udp_server.h"

#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/net/udp/network_frame.h"

#include <algorithm>
#include <utility>

// Server 音频实时链诊断开关：broadcast 跑在 paced dispatcher worker 线程上。
#ifndef AQUA_SERVER_RT_DEBUG_LOG
#define AQUA_SERVER_RT_DEBUG_LOG 0
#endif

namespace aqua::net {

UdpServer::State::State(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sess)
    : v4_transport(std::make_shared<UdpTransport>(ioc))
    , v6_transport(std::make_shared<UdpTransport>(ioc))
    , sessions(std::move(sess))
{
}

UdpServer::UdpServer(asio::io_context& ioc, std::shared_ptr<session::SessionManager> sessions)
    : state_(std::make_shared<State>(ioc, std::move(sessions)))
{
    log_debug("UdpServer created");
}

UdpServer::~UdpServer()
{
    stop();
}

UdpBindOutcome UdpServer::bind(const UdpBindPlan& plan)
{
    const auto st = state_;
    UdpBindOutcome outcome;
    log_debug_fmt("UdpServer bind requested: v4={} v6={} port={}",
        plan.v4 ? plan.v4->address : std::string("<disabled>"),
        plan.v6 ? plan.v6->address : std::string("<disabled>"), plan.port);

    // 端口 0 = 让 OS 选**一次**，两族共用同一个 —— plan 的契约就是"两族同端口"，
    // 所以 v6 必须跟随 v4 实际拿到的端口，而不是各自要一个临时端口。
    std::uint16_t v6_port = plan.port;

    if (plan.v4) {
        if (const auto bound = st->v4_transport->bind(plan.v4->address, plan.port); !bound) {
            outcome.v4_error = bound.error();
            log_error_fmt("UdpServer: IPv4 data plane bind failed on {}: {}",
                format_host_port(plan.v4->address, plan.port), net_error_name(bound.error()));
        } else {
            st->v4_bound.store(true, std::memory_order_release);
            outcome.served.v4 = true;
            if (plan.port == 0) {
                v6_port = st->v4_transport->local_endpoint().port();
            }
            log_info_fmt("UdpServer IPv4 data plane bound on {} (AF_INET socket)",
                format_host_port(plan.v4->address, plan.port));
        }
    }
    if (plan.v6) {
        if (const auto bound = st->v6_transport->bind(plan.v6->address, v6_port); !bound) {
            outcome.v6_error = bound.error();
            log_error_fmt("UdpServer: IPv6 data plane bind failed on {}: {}",
                format_host_port(plan.v6->address, plan.port), net_error_name(bound.error()));
        } else {
            st->v6_bound.store(true, std::memory_order_release);
            outcome.served.v6 = true;
            // v6_only=true 由 UdpTransport 保证：这是"两族同端口共存"的前提，
            // 也是"原生 v6 与 ::ffff: 两种表示不混入系统"的前提。
            log_info_fmt("UdpServer IPv6 data plane bound on {} (v6_only=true)",
                format_host_port(plan.v6->address, plan.port));
        }
    }

    log_info_fmt("UdpServer data plane bind outcome: served={} v4_failed={} v6_failed={}",
        outcome.served.to_string(), outcome.v4_error.has_value() ? "yes" : "no",
        outcome.v6_error.has_value() ? "yes" : "no");
    return outcome;
}

std::expected<void, NetError> UdpServer::bind(const std::string& bind_ip, std::uint16_t port)
{
    asio::ip::address address {};
    try {
        address = ::aqua::net::parse_ip_address(bind_ip);
    } catch (const std::exception& e) {
        log_error_fmt("UdpServer::bind rejected invalid address {} - {}", bind_ip,
            format_exception_message(e));
        return std::unexpected(NetError::InvalidEndpoint);
    }

    UdpBindTarget target { bind_ip, true };
    UdpBindPlan plan;
    plan.port = port;
    // 族判定用**原生形态**（不是 effective_family）：::ffff:a.b.c.d 归一化后算 v4，
    // 但它的文本形态会让 UdpTransport 开出 AF_INET6 socket —— 那与"放进 v4 槽"矛盾。
    // 这类地址没有自洽的归宿，直接拒绝：本适配器只服务"写清楚是哪一族"的调用方。
    if (is_native_family(address, IpFamily::V6)) {
        plan.v6 = std::move(target);
    } else if (is_native_family(address, IpFamily::V4)) {
        plan.v4 = std::move(target);
    } else {
        log_error_fmt("UdpServer::bind rejected {}: a v4-mapped IPv6 literal has no consistent "
                      "address family (write the plain IPv4 address instead)",
            bind_ip);
        return std::unexpected(NetError::InvalidEndpoint);
    }

    const auto outcome = bind(plan);
    if (plan.v4 && outcome.v4_error) {
        return std::unexpected(*outcome.v4_error);
    }
    if (plan.v6 && outcome.v6_error) {
        return std::unexpected(*outcome.v6_error);
    }
    if (outcome.served.empty()) {
        return std::unexpected(NetError::BindFailed);
    }
    return { };
}

std::expected<void, NetError> UdpServer::start()
{
    const auto st = state_;
    if (st->served().empty()) {
        log_error("UdpServer::start rejected: no data-plane family is bound");
        return std::unexpected(NetError::NotOpen);
    }

    for (const auto family : { IpFamily::V4, IpFamily::V6 }) {
        const auto transport = st->transport_for(family);
        if (!transport) {
            continue;
        }
        const auto local = transport->local_endpoint();
        log_debug_fmt("UdpServer receive configuration: family={} local={}",
            ip_family_name(family),
            format_host_port(local.address().to_string(), local.port()));

        // handler 捕获"它自己那条 transport"与自己的族：ACK 必须从收到包的同一条
        // socket 回去。NAT 映射绑定在 (本地端口, 远端, 协议族) 上，换一条 socket
        // 回包会直接丢；族则显式下传（不从地址反推），供 SessionManager 做族粘性判断。
        const std::weak_ptr<State> weak_st = st;
        const auto started = transport->start_receive(
            [weak_st, transport, family](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
                const auto st = weak_st.lock();
                if (!st) {
                    return;
                }
                const auto frame = NetworkFrame::decode(data);
                if (!frame) {
                    st->malformed_datagrams.fetch_add(1, std::memory_order_relaxed);
                    log_trace_fmt("UdpServer ignored malformed datagram: bytes={}", data.size());
                    return;
                }
                if (frame->type() == PacketType::Heartbeat) {
                    // client→server 唯一包型：首包建立 association（Created→Connected），
                    // 之后只刷新 endpoint（NAT 重绑/漫游时静默跟随，不碰 last_seen）。
                    // 每个合法 heartbeat 都回 HeartbeatAck：首包是建连确认，之后是
                    // 路径探活回执（client 两阶段都做 ACK 跟踪，无 ACK 即路径死亡）。
                    // 未知 session 一律拒绝（不回 ACK）。
                    st->heartbeat_received.fetch_add(1, std::memory_order_relaxed);
                    if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
                        log_trace_fmt("UdpServer heartbeat received: session=0x{:08X} sender={} bytes={}",
                            frame->session_id(), sender.address().to_string(), data.size());
                    }
                    using aqua::session::SessionManager;
                    switch (st->sessions->on_heartbeat(frame->session_id(), sender, family)) {
                    case SessionManager::HeartbeatOutcome::Rejected:
                        st->heartbeat_rejected.fetch_add(1, std::memory_order_relaxed);
                        if (aqua::log_level_enabled(aqua::LogLevel::Debug)) {
                            log_debug_fmt("UDP heartbeat rejected: session=0x{:08X} sender={}",
                                frame->session_id(), sender.address().to_string());
                        }
                        return;
                    case SessionManager::HeartbeatOutcome::Refreshed:
                        if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
                            log_trace_fmt("UDP heartbeat refreshed: session=0x{:08X} sender={}",
                                frame->session_id(), sender.address().to_string());
                        }
                        break; // 落到下方统一回 ACK（路径探活回执）
                    case SessionManager::HeartbeatOutcome::Established:
                        log_info_fmt("UDP session established: session=0x{:08X} sender={}",
                            frame->session_id(), sender.address().to_string());
                        break;
                    }
                    const auto ack = NetworkFrame::heartbeat_ack(frame->session_id()).encode();
                    transport->send_to(sender, ack);
                    st->heartbeat_ack_attempts.fetch_add(1, std::memory_order_relaxed);
                    log_trace_fmt("UDP heartbeat ACK queued: session=0x{:08X}", frame->session_id());
                    return;
                }
                st->non_heartbeat_datagrams.fetch_add(1, std::memory_order_relaxed);
                log_trace_fmt("UdpServer ignored non-heartbeat datagram: bytes={}", data.size());
                return;
            });
        if (!started) {
            // 留下"绑定了但收不到"的半个数据面是最难诊断的状态（客户端会一直
            // 收不到 ACK 而服务端毫无察觉），因此整体回滚并让上层启动失败。
            log_error_fmt("UdpServer: failed to start receive loop for {}: {}", ip_family_name(family),
                net_error_name(started.error()));
            stop();
            return started;
        }
    }

    log_debug_fmt("UdpServer receive loops started: served={}", st->served().to_string());
    return { };
}

void UdpServer::stop() noexcept
{
    log_debug("UdpServer stop requested");
    const auto st = state_;
    st->v4_transport->stop();
    st->v6_transport->stop();
}

std::optional<std::size_t> UdpServer::broadcast(std::shared_ptr<const std::vector<std::byte>> datagram) noexcept
{
    if (!datagram || datagram->empty()) {
        return std::nullopt;
    }

    try {
        const auto st = state_;
        st->sessions->snapshot_connected(connected_scratch_);
#if AQUA_SERVER_RT_DEBUG_LOG
        std::vector<asio::ip::udp::endpoint> delivered_endpoints;
        delivered_endpoints.reserve(connected_scratch_.size());
#endif
        std::size_t delivered = 0;
        for (const auto& session : connected_scratch_) {
            // 路由键 = 会话记录的族。它与 endpoint 由同一条 socket 在同一把锁内
            // 一起写入（INV-2），因此永不发散；换 socket 发既会打破 NAT 映射，
            // 也可能直接 EAFNOSUPPORT。
            const auto transport = st->transport_for(session.family);
            if (!transport) {
                // 该族数据面未服务（理论上不该存在这样的 session）；跳过而不是
                // 硬发，否则会在 AF_INET6 socket 上用 AF_INET 目标抛 EAFNOSUPPORT。
                log_trace_fmt("UdpServer broadcast skipped session 0x{:08X}: its family is not served",
                    session.session_id);
                continue;
            }
            transport->send_to_shared(session.endpoint, datagram);
            ++delivered;
#if AQUA_SERVER_RT_DEBUG_LOG
            delivered_endpoints.push_back(session.endpoint);
#endif
        }
#if AQUA_SERVER_RT_DEBUG_LOG
        // 接收端集合变化才打（首播 + 成员增减/漫游）：逐包打地址 274Hz×N 没人看，
        // 逐包计数由 dispatcher 的 sent 行覆盖。
        {
            bool changed = delivered_endpoints.size() != last_broadcast_endpoints_.size();
            if (!changed) {
                for (std::size_t i = 0; i < delivered_endpoints.size(); ++i) {
                    if (delivered_endpoints[i] != last_broadcast_endpoints_[i]) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) {
                last_broadcast_endpoints_ = delivered_endpoints;
                std::string endpoints;
                for (const auto& ep : delivered_endpoints) {
                    if (!endpoints.empty()) {
                        endpoints += ',';
                    }
                    endpoints += format_host_port(ep.address().to_string(), ep.port());
                }
                log_debug_fmt("UdpServer broadcast recipients changed: n={} [{}]",
                    delivered_endpoints.size(), endpoints);
            }
        }
#endif
        return delivered;
    } catch (const std::exception& e) {
        log_warn_fmt("UdpServer::broadcast failed: {}", format_exception_message(e));
        return std::nullopt;
    } catch (...) {
        log_warn("UdpServer::broadcast failed: unknown exception");
        return std::nullopt;
    }
}

UdpTransportStats UdpServer::stats(IpFamily family) const noexcept
{
    const auto st = state_;
    const auto transport = st->transport_for(family);
    return transport ? transport->stats() : UdpTransportStats { };
}

UdpTransportStats UdpServer::stats() const noexcept
{
    // 两族合计：计数逐项相加；tx_queue_depth 是瞬时量，取较大者（相加无意义）。
    auto total = stats(IpFamily::V4);
    const auto v6 = stats(IpFamily::V6);
    total.rx_packets += v6.rx_packets;
    total.rx_bytes += v6.rx_bytes;
    total.rx_errors += v6.rx_errors;
    total.rx_unreachable += v6.rx_unreachable;
    total.tx_packets += v6.tx_packets;
    total.tx_bytes += v6.tx_bytes;
    total.tx_errors += v6.tx_errors;
    total.tx_dropped += v6.tx_dropped;
    total.tx_enqueue_failures += v6.tx_enqueue_failures;
    total.tx_queue_depth = std::max(total.tx_queue_depth, v6.tx_queue_depth);
    return total;
}

asio::ip::udp::endpoint UdpServer::local_endpoint(IpFamily family) const noexcept
{
    const auto st = state_;
    const auto transport = st->transport_for(family);
    return transport ? transport->local_endpoint() : asio::ip::udp::endpoint { };
}

asio::ip::udp::endpoint UdpServer::local_endpoint() const noexcept
{
    const auto st = state_;
    if (st->v4_bound.load(std::memory_order_acquire)) {
        return st->v4_transport->local_endpoint();
    }
    if (st->v6_bound.load(std::memory_order_acquire)) {
        return st->v6_transport->local_endpoint();
    }
    return { };
}

FamilySet UdpServer::served_families() const noexcept
{
    return state_->served();
}

std::uint64_t UdpServer::heartbeat_received() const noexcept { return state_->heartbeat_received.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::heartbeat_rejected() const noexcept { return state_->heartbeat_rejected.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::sessions_established() const noexcept { return state_->sessions->stats().connected; }
std::uint64_t UdpServer::sessions_refreshed() const noexcept { return state_->sessions->stats().refreshed; }
std::uint64_t UdpServer::heartbeat_ack_attempts() const noexcept { return state_->heartbeat_ack_attempts.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::malformed_datagrams() const noexcept { return state_->malformed_datagrams.load(std::memory_order_relaxed); }
std::uint64_t UdpServer::non_heartbeat_datagrams() const noexcept { return state_->non_heartbeat_datagrams.load(std::memory_order_relaxed); }

} // namespace aqua::net
