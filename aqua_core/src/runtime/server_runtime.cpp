#include "aqua/runtime/server_runtime.h"

#include "aqua/net/udp/udp_packet.h"

namespace aqua::runtime {

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
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
    return udp_.start_receive(
        [this](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
            handle_datagram(sender, data);
        });
}

void ServerRuntime::stop()
{
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

} // namespace aqua::runtime
