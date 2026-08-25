#include "aqua/runtime/server_runtime.h"

#include "aqua/net/udp/udp_packet.h"

namespace aqua::runtime {

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , sessions_()
    , udp_(ioc)
    , hello_(sessions_, on_ack, this)
    , packetizer_(config.frames_per_slot, config.format.frame_bytes())
{
}

ServerRuntime::~ServerRuntime() = default;

bool ServerRuntime::start()
{
    if (!udp_.bind(config_.udp_bind_ip, config_.udp_port)) {
        return false;
    }
    // 捕获 shared_from_this：UdpSocketBase::stop 只 post close_state、不等待 strand 排空，
    // transport State 可能在 runtime 析构后仍短暂持有收包 handler；绑定自身生命周期避免悬垂。
    auto self = shared_from_this();
    if (!udp_.start_receive(
            [self](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
                self->handle_datagram(sender, data);
            })) {
        return false;
    }
    reap_timer_ = std::make_unique<asio::steady_timer>(ioc_);
    schedule_reap();
    return true;
}

void ServerRuntime::stop()
{
    if (reap_timer_ != nullptr) {
        asio::error_code ec;
        reap_timer_->cancel(ec);
        reap_timer_.reset();
    }
    udp_.stop();
}

void ServerRuntime::push_pcm(std::span<const std::byte> pcm) noexcept
{
    packetizer_.push(pcm, on_packetized, this);
}

void ServerRuntime::handle_datagram(const asio::ip::udp::endpoint& sender,
    std::span<const std::byte> data) noexcept
{
    hello_.handle(sender, data);
}

void ServerRuntime::on_packetized(void* ud, std::uint64_t sequence,
    std::span<const std::byte> pcm) noexcept
{
    auto* rt = static_cast<ServerRuntime*>(ud);
    auto packet = std::make_shared<const std::vector<std::byte>>(
        net::encode_audio_packet(sequence, pcm));
    rt->broadcast(std::move(packet));
}

void ServerRuntime::on_ack(void* ud, const asio::ip::udp::endpoint& target,
    std::span<const std::byte> ack) noexcept
{
    static_cast<ServerRuntime*>(ud)->udp_.send_copy(target, ack);
}

void ServerRuntime::broadcast(std::shared_ptr<const std::vector<std::byte>> packet) noexcept
{
    std::vector<SessionManager::ConnectedSession> connected;
    sessions_.snapshot_connected(connected);
    for (const auto& session : connected) {
        udp_.send_shared(session.endpoint, packet);
    }
    frames_broadcast_.fetch_add(1, std::memory_order_relaxed);
}

void ServerRuntime::schedule_reap()
{
    if (reap_timer_ == nullptr) {
        return;
    }
    reap_timer_->expires_after(config_.session_reap_interval);
    auto self = shared_from_this();
    reap_timer_->async_wait([self](const asio::error_code& ec) {
        if (ec) {
            return; // cancelled / 已 stop
        }
        self->sessions_.remove_expired_sessions(self->config_.session_timeout);
        self->schedule_reap();
    });
}

} // namespace aqua::runtime
