#include "aqua/runtime/server_runtime.h"

namespace aqua::runtime {

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , sessions_(std::make_shared<session::SessionManager>())
    , udp_(ioc, sessions_)
    , packetizer_(config.frame_count, config.format.frame_bytes())
{
    // packetize → 协议层 encode + 广播（wire 格式知识不进入 runtime）。
    packetize_handler_ = [this](const audio::AudioFrame& frame) noexcept {
        udp_.send_audio(frame);
    };
}

ServerRuntime::~ServerRuntime()
{
    stop();
}

bool ServerRuntime::start()
{
    if (!udp_.bind(config_.udp_bind_ip, config_.udp_port)) {
        return false;
    }
    if (!udp_.start()) {
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
    packetizer_.push(pcm, packetize_handler_);
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
        self->sessions_->remove_expired_sessions(self->config_.session_timeout);
        self->schedule_reap();
    });
}

} // namespace aqua::runtime
