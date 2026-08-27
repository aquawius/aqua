#include "aqua/runtime/client_runtime.h"

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

bool ClientRuntime::start()
{
    if (started_ || stopped_) {
        return false;
    }

    device_mgr_ = audio::create_device_manager();
    if (!device_mgr_) {
        return false;
    }
    playback_ = audio::create_playback(*device_mgr_);
    if (!playback_) {
        return false;
    }

    // 控制面：gRPC Connect 拿 session/format，建 JB，设 UDP 远端。
    connect_result_ = {};
    if (!grpc_.connect_to_server(config_.server_ip, config_.rpc_port)) {
        return false;
    }
    if (!grpc_.connect(config_.client_name, connect_result_)) {
        return false;
    }
    if (!setup_playback(connect_result_.audio_format, connect_result_.frame_count)) {
        // server 侧 session 已创建，本地 setup 失败 → best-effort 清理，避免残留 session。
        (void)grpc_.disconnect(connect_result_.session_id);
        return false;
    }
    if (!udp_.set_remote(connect_result_.udp_address, connect_result_.udp_port)) {
        (void)grpc_.disconnect(connect_result_.session_id);
        return false;
    }

    // 数据面：UDP 接收（Audio 帧 → JB）+ 周期 HELLO。
    const auto expected_payload_bytes = static_cast<std::size_t>(frame_count_) * frame_bytes_;
    if (!udp_.start_receive(expected_payload_bytes,
            [jb = jb_, frame_count = frame_count_](std::uint64_t sequence,
                std::span<const std::byte> pcm) {
                const audio::AudioFrame frame { sequence, frame_count, pcm };
                (void)jb->push(frame);
            })) {
        (void)grpc_.disconnect(connect_result_.session_id);
        return false;
    }
    if (connect_result_.is_valid()) {
        udp_.start_hello(connect_result_.session_id, config_.hello_interval);
    }

    // 回放：format 以 server 下发为准。回调捕获裸 this（stop() 先 join 音频线程再析构成员）。
    auto pb_cfg = config_.playback;
    pb_cfg.format = connect_result_.audio_format;
    if (!playback_->start(pb_cfg, [this](std::span<std::byte> output) noexcept {
            return pull_playback(output);
        })) {
        udp_.stop();
        return false;
    }

    started_ = true;
    return true;
}

void ClientRuntime::stop()
{
    if (stopped_) {
        return;
    }
    stopped_ = true;

    // 先停回放（join 音频线程，不再有 pull 回调），再停 udp + best-effort Disconnect。
    if (playback_) {
        playback_->stop();
    }
    udp_.stop();
    if (connect_result_.is_valid()) {
        (void)grpc_.disconnect(connect_result_.session_id);
    }
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frame_count)
{
    if (started_ || stopped_ || !format.is_valid() || frame_count == 0) {
        return false;
    }
    frame_count_ = frame_count;
    frame_bytes_ = format.frame_bytes();
    audio::JitterBufferConfig cfg;
    cfg.capacity_slots = config_.jitter_buffer_slots;
    cfg.format = format;
    cfg.frame_count = frame_count;
    auto jb = audio::JitterBuffer::create(cfg);
    if (!jb) {
        jb_.reset();
        return false;
    }
    jb_ = std::move(*jb);
    return true;
}

std::uint32_t ClientRuntime::pull_playback(std::span<std::byte> output) noexcept
{
    if (jb_ == nullptr) {
        return 0;
    }
    return jb_->pull(output).frames_filled;
}

} // namespace aqua::runtime
