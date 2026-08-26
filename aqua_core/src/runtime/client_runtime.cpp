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
    return true;
}

bool ClientRuntime::setup_playback(const audio::AudioFormat& format,
    std::uint32_t frames_per_slot)
{
    frames_per_slot_ = frames_per_slot;
    audio::JitterBufferConfig cfg;
    cfg.capacity_slots = config_.jitter_buffer_slots;
    cfg.format = format;
    cfg.frames_per_slot = frames_per_slot;
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

bool ClientRuntime::start()
{
    if (jb_ == nullptr || frames_per_slot_ == 0) {
        return false;
    }
    // 收包回调只捕获 JB 的 shared_ptr：即使 runtime 析构后 strand 上仍有排队的
    // 收包事件，JB 也保持存活，无 UAF（无需 shared_from_this）。
    if (!udp_.start(frames_per_slot_, [jb = jb_](const audio::AudioFrame& frame) {
            jb->push(frame);
        })) {
        return false;
    }
    if (connect_result_.is_valid()) {
        udp_.start_hello(connect_result_.session_id, config_.hello_interval);
    }
    return true;
}

void ClientRuntime::stop()
{
    udp_.stop();
}

} // namespace aqua::runtime
