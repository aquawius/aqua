#include "aqua/runtime/server_runtime.h"

namespace aqua::runtime {

ServerRuntime::ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config)
    : config_(config)
    , ioc_(ioc)
    , sessions_(std::make_shared<session::SessionManager>())
    , udp_(ioc, sessions_)
    , packetizer_(config.frame_count, config.format.frame_bytes())
    , frame_queue_(config.network_queue_slots, config.frame_count, config.format.frame_bytes())
    , dispatcher_(frame_queue_, udp_)
{
}

ServerRuntime::~ServerRuntime()
{
    stop();
}

bool ServerRuntime::start()
{
    RuntimeState expected = RuntimeState::Created;
    if (!state_.compare_exchange_strong(expected, RuntimeState::Starting,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    if (!config_.format.is_valid() || config_.frame_count == 0
        || config_.network_queue_slots == 0 || !packetizer_.valid()) {
        stop();
        return false;
    }

    // 设备 + 采集后端（经工厂，不依赖平台实现）。
    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        stop();
        return false;
    }
    capture_ = audio::create_capture(*device_mgr_);
    if (!capture_) {
        stop();
        return false;
    }

    // 数据面：udp + dispatcher（reap 定时器稍后建）。
    if (!udp_.bind(config_.udp_bind_ip, config_.udp_port)) {
        stop();
        return false;
    }
    if (!udp_.start()) {
        stop();
        return false;
    }
    if (!dispatcher_.start()) {
        stop();
        return false;
    }

    // 控制面 gRPC（构造即 BuildAndStart；is_started() 不依赖 run 线程是否进入 Wait()）。
    grpc_ = std::make_unique<grpc::GrpcServer>(
        *sessions_, config_.format, config_.frame_count,
        config_.rpc_bind_ip, config_.rpc_port,
        grpc::AdvertisedUdpEndpoint { config_.advertised_udp_address, config_.udp_port });
    if (!grpc_->is_started()) {
        stop();
        return false;
    }
    grpc_thread_ = std::thread([this] { grpc_->run(); });

    // 采集：format 与 packetizer 对齐。回调捕获裸 this（stop() 先 join 音频线程再析构成员，无 UAF）。
    auto capture_cfg = config_.capture;
    capture_cfg.format = config_.format;
    if (!capture_->start(capture_cfg,
            [this](const audio::AudioBlock& block) noexcept {
                on_capture_block(block);
            },
            [this](audio::AudioError error) noexcept {
                on_capture_event(error);
            })) {
        stop();
        return false;
    }

    reap_timer_ = std::make_unique<asio::steady_timer>(ioc_);
    if (state_.load(std::memory_order_acquire) == RuntimeState::Starting) {
        state_.store(RuntimeState::Running, std::memory_order_release);
    }
    schedule_reap();
    return true;
}

void ServerRuntime::stop()
{
    const auto current = state_.load(std::memory_order_acquire);
    if (current == RuntimeState::Stopped || current == RuntimeState::Stopping) {
        return;
    }
    state_.store(RuntimeState::Stopping, std::memory_order_release);

    // 顺序即契约：先停采集（join 音频线程，不再 push），再停 dispatcher（drain 剩余帧）
    // 与 udp，最后停 gRPC。
    if (capture_) {
        capture_->stop();
    }
    if (reap_timer_ != nullptr) {
        asio::error_code ec;
        reap_timer_->cancel(ec);
        reap_timer_.reset();
    }
    dispatcher_.stop();
    udp_.stop();
    sessions_->clear();
    if (grpc_) {
        grpc_->shutdown();
    }
    if (grpc_thread_.joinable()) {
        grpc_thread_.join();
    }
    state_.store(RuntimeState::Stopped, std::memory_order_release);
}

void ServerRuntime::on_capture_block(const audio::AudioBlock& block) noexcept
{
    const auto state = state_.load(std::memory_order_acquire);
    if ((state != RuntimeState::Starting && state != RuntimeState::Running
            && state != RuntimeState::Degraded) || block.data.empty()) {
        return;
    }

    packetizer_.push(block.data, [this](const audio::AudioFrame& frame) noexcept {
        const auto result = frame_queue_.push(frame);
        if (result.accepted && result.was_empty) {
            dispatcher_.notify_from_realtime();
        }
    });
}

void ServerRuntime::on_capture_event(audio::AudioError error) noexcept
{
    last_audio_error_.store(error, std::memory_order_relaxed);
    auto expected = RuntimeState::Starting;
    if (!state_.compare_exchange_strong(expected, RuntimeState::Degraded,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        expected = RuntimeState::Running;
        state_.compare_exchange_strong(expected, RuntimeState::Degraded,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

void ServerRuntime::schedule_reap()
{
    const auto state = state_.load(std::memory_order_acquire);
    if (reap_timer_ == nullptr || state == RuntimeState::Stopping
        || state == RuntimeState::Stopped) {
        return;
    }
    reap_timer_->expires_after(config_.session_reap_interval);
    auto self = shared_from_this();
    reap_timer_->async_wait([self](const asio::error_code& ec) {
        const auto state = self->state_.load(std::memory_order_acquire);
        if (ec || state == RuntimeState::Stopping || state == RuntimeState::Stopped) {
            return;
        }
        self->sessions_->remove_expired_sessions(self->config_.session_timeout);
        self->schedule_reap();
    });
}

} // namespace aqua::runtime
