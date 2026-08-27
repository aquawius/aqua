#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 侧的统领（唯一入口）。
//
// Lifecycle is one-shot: Created -> Starting -> Running/Degraded -> Stopping -> Stopped.
// start() and stop() are control-plane operations and must not run concurrently; stop() is
// idempotent and may be called from the owner/control thread repeatedly.

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/runtime/runtime_state.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace aqua::runtime {

struct ClientRuntimeConfig {
    std::uint32_t jitter_buffer_slots = 30;
    std::chrono::milliseconds hello_interval { config::HELLO_INTERVAL };
    audio::AudioPlaybackConfig playback;
    std::string server_ip = "127.0.0.1";
    std::uint16_t rpc_port = 50051;
    std::string client_name = "aqua-client";
};

class ClientRuntime final {
public:
    ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    bool start();
    void stop() noexcept;

    [[nodiscard]] RuntimeState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] audio::AudioError last_audio_error() const noexcept
    {
        return last_audio_error_.load(std::memory_order_acquire);
    }
    const grpc::ConnectResult& connect_result() const noexcept { return connect_result_; }
    audio::JitterBuffer* jitter_buffer() noexcept { return jb_.get(); }
    [[nodiscard]] bool playback_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

private:
    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frame_count);
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;
    bool enter_starting() noexcept;
    bool enter_stopping() noexcept;
    void enter_stopped() noexcept;
    void on_playback_event(audio::AudioError error) noexcept;

    ClientRuntimeConfig config_;
    asio::io_context& ioc_;
    std::unique_ptr<audio::AudioDeviceManager> device_mgr_;
    std::unique_ptr<audio::AudioPlayback> playback_;
    grpc::GrpcClient grpc_;
    net::UdpClient udp_;
    std::shared_ptr<audio::JitterBuffer> jb_;
    std::uint32_t frame_count_ = 0;
    std::uint32_t frame_bytes_ = 0;
    grpc::ConnectResult connect_result_;
    std::atomic<RuntimeState> state_ { RuntimeState::Created };
    std::atomic<audio::AudioError> last_audio_error_ { audio::AudioError::None };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
