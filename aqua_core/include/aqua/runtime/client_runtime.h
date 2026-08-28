#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 侧的统领（唯一入口）。
//
// 生命周期是一次性的：Created -> Starting -> Running/Degraded -> Stopping -> Stopped。
// start() 与 stop() 是控制面操作，不得并发执行；stop() 幂等，
// 可由属主/控制线程重复调用。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/logger/logger.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/runtime/runtime_state.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <cstdint>
#include <memory>
#include <mutex>
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
    [[nodiscard]] double jitter_water_level() const noexcept;
    [[nodiscard]] std::uint32_t jitter_used_slots() const noexcept;
    [[nodiscard]] std::uint32_t jitter_capacity_slots() const noexcept;
    [[nodiscard]] std::uint64_t jitter_reanchor_count() const noexcept;
    [[nodiscard]] std::uint64_t jitter_reanchor_sanity_rejections() const noexcept;
    [[nodiscard]] std::uint64_t jitter_last_reanchor_sequence() const noexcept;
    [[nodiscard]] std::uint64_t hello_ack_count() const noexcept { return udp_.hello_ack_count(); }
    [[nodiscard]] std::uint32_t hello_ack_misses() const noexcept { return udp_.consecutive_hello_ack_misses(); }
    [[nodiscard]] std::int64_t hello_ack_age_ms() const noexcept { return udp_.hello_ack_age_ms(); }
    [[nodiscard]] bool udp_hello_failed() const noexcept { return udp_.hello_failed(); }
    [[nodiscard]] std::uint64_t udp_tx_enqueue_failures() const noexcept
    {
        return udp_.stats().tx_enqueue_failures;
    }
    [[nodiscard]] bool playback_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

private:
    struct CallbackGate {
        explicit CallbackGate(ClientRuntime* owner) noexcept : owner(owner) {}

        CallbackGate(const CallbackGate&) = delete;
        CallbackGate& operator=(const CallbackGate&) = delete;

        template <typename Fn>
        void invoke(Fn&& fn) noexcept
        {
            std::lock_guard lock(mutex);
            if (owner != nullptr) {
                try {
                    fn(*owner);
                } catch (const std::exception& e) {
                    log_error_fmt("ClientRuntime asynchronous notification callback threw: {}", e.what());
                } catch (...) {
                    log_error("ClientRuntime asynchronous notification callback threw unknown exception");
                }
            }
        }

        void detach() noexcept
        {
            std::lock_guard lock(mutex);
            owner = nullptr;
        }

        std::mutex mutex;
        ClientRuntime* owner = nullptr;
    };

    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frame_count);
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;
    bool enter_starting() noexcept;
    bool enter_stopping() noexcept;
    void enter_stopped() noexcept;
    void on_playback_event(audio::AudioError error) noexcept;
    void on_network_liveness_failure(std::uint32_t consecutive_misses) noexcept;
    void on_reanchor_sanity_failure(std::uint64_t rejections) noexcept;

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
    std::shared_ptr<CallbackGate> callback_gate_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
