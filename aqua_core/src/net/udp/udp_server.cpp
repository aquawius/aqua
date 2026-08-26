#include "aqua/net/udp/udp_server.h"

#include "aqua/net/udp/network_frame.h"

#include <utility>
#include <vector>

namespace aqua::net {

UdpServer::State::State(asio::io_context& ioc, std::shared_ptr<SessionManager> sess)
    : transport(std::make_shared<UdpTransport>(ioc))
    , sessions(std::move(sess))
{
}

UdpServer::UdpServer(asio::io_context& ioc, std::shared_ptr<SessionManager> sessions)
    : state_(std::make_shared<State>(ioc, std::move(sessions)))
{
}

UdpServer::~UdpServer()
{
    stop();
}

bool UdpServer::bind(const std::string& bind_ip, std::uint16_t port)
{
    return state_->transport->bind(bind_ip, port);
}

bool UdpServer::start()
{
    const auto& st = state_;
    // 收包 handler 只捕获共享 State：即使 UdpServer 析构后 strand 上仍有
    // 排队的收包完成事件，State（及 SessionManager）也保持存活，无 UAF。
    return st->transport->start_receive(
        [st](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            const auto frame = NetworkFrame::decode(data);
            if (!frame || frame->type() != PacketType::Hello) {
                return; // Audio / HelloAck / malformed：server 数据面只认 HELLO
            }
            // HELLO：学习/刷新 NAT 映射 endpoint 并置 Connected（幂等）。
            if (!st->sessions->establish_session(frame->session_id(), sender)) {
                return; // 未知 session 或非法 endpoint，不回复
            }
            const auto ack = NetworkFrame::hello_ack(frame->session_id()).encode();
            st->transport->send_to(sender, ack);
        });
}

void UdpServer::stop() noexcept
{
    state_->transport->stop();
}

void UdpServer::send_audio(const audio::AudioFrame& frame) noexcept
{
    const auto& st = state_;
    try {
        // 一份共享 wire 缓冲广播给所有 Connected session，避免逐 session 拷贝。
        auto packet = std::make_shared<const std::vector<std::byte>>(
            NetworkFrame::audio(frame.sequence, frame.data).encode());
        std::vector<SessionManager::ConnectedSession> connected;
        st->sessions->snapshot_connected(connected);
        for (const auto& session : connected) {
            st->transport->send_to_shared(session.endpoint, packet);
        }
    } catch (...) {
        // 分配失败（bad_alloc 等）不允许传出：调用方是实时采集线程。
    }
    st->frames_broadcast.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t UdpServer::frames_broadcast() const noexcept
{
    return state_->frames_broadcast.load(std::memory_order_relaxed);
}

UdpTransportStats UdpServer::stats() const noexcept
{
    return state_->transport->stats();
}

asio::ip::udp::endpoint UdpServer::local_endpoint() const noexcept
{
    return state_->transport->socket_local_endpoint();
}

} // namespace aqua::net
