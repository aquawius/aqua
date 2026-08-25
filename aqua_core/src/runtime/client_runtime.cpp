#include "aqua/runtime/client_runtime.h"

#include "aqua/net/udp/udp_packet.h"

namespace aqua::runtime {

ClientRuntime::ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , grpc_()
    , udp_(ioc)
{
}

ClientRuntime::~ClientRuntime()
{
    stop();
}

bool ClientRuntime::connect(const std::string& server_ip, std::uint16_t rpc_port,
    const std::string& client_name)
{
    if (!grpc_.connect_to_server(server_ip, rpc_port)) {
        return false;
    }
    if (!grpc_.connect(client_name, connect_result_)) {
        return false;
    }
    if (!setup_playback(connect_result_.audio_format, connect_result_.frames_per_slot)) {
        // server 侧 session 已创建，本地 setup 失败 → best-effort 清理，避免残留 session。
        (void)grpc_.disconnect(connect_result_.session_id);
        return false;
    }
    if (!udp_.set_remote(connect_result_.udp_address, connect_result_.udp_port)) {
        (void)grpc_.disconnect(connect_result_.session_id);
        return false;
    }
    hello_sender_ = std::make_unique<HelloSender>(
        connect_result_.session_id, on_send_hello, this);
    return true;
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frames_per_slot)
{
    audio::JitterBufferConfig cfg;
    cfg.capacity_slots = config_.jitter_buffer_slots;
    cfg.format = format;
    cfg.frames_per_slot = frames_per_slot;
    auto jb = audio::JitterBuffer::create(cfg);
    if (!jb) {
        jb_.reset();
        depacketizer_.reset();
        return false;
    }
    jb_ = std::move(*jb);
    depacketizer_ = std::make_unique<audio::AudioDepacketizer>(*jb_, frames_per_slot);
    return true;
}

void ClientRuntime::handle_datagram(const asio::ip::udp::endpoint& /*sender*/,
    std::span<const std::byte> data) noexcept
{
    if (depacketizer_ != nullptr) {
        depacketizer_->handle_datagram(data);
    }
}

std::uint32_t ClientRuntime::pull_playback(std::span<std::byte> output) noexcept
{
    if (jb_ == nullptr) {
        return 0;
    }
    return jb_->pull(output).frames_filled;
}

bool ClientRuntime::start()
{
    if (!connect_result_.is_valid()) {
        return false;
    }
    if (!udp_.open()) {
        return false;
    }
    auto self = shared_from_this();
    if (!udp_.start_receive(
            [self](const asio::ip::udp::endpoint& sender, std::span<const std::byte> data) {
                self->handle_datagram(sender, data);
            })) {
        return false;
    }
    hello_timer_ = std::make_unique<asio::steady_timer>(ioc_);
    schedule_hello();
    return true;
}

void ClientRuntime::stop()
{
    if (hello_timer_ != nullptr) {
        asio::error_code ec;
        hello_timer_->cancel(ec);
        hello_timer_.reset();
    }
    udp_.stop();
}

void ClientRuntime::on_send_hello(void* ud, std::span<const std::byte> packet) noexcept
{
    static_cast<ClientRuntime*>(ud)->udp_.send(packet);
}

void ClientRuntime::schedule_hello()
{
    if (hello_timer_ == nullptr) {
        return;
    }
    hello_timer_->expires_after(config_.hello_interval);
    auto self = shared_from_this();
    hello_timer_->async_wait([self](const asio::error_code& ec) {
        if (ec) {
            return; // cancelled / 已 stop
        }
        if (self->hello_sender_ != nullptr) {
            self->hello_sender_->send();
        }
        self->schedule_hello();
    });
}

} // namespace aqua::runtime
