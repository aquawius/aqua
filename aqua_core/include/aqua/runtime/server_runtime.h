#ifndef AQUA_RUNTIME_SERVER_RUNTIME_H
#define AQUA_RUNTIME_SERVER_RUNTIME_H

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/audio/queue/audio_frame_queue.h"
#include "aqua/net/grpc/grpc_server.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/runtime/audio_network_dispatcher.h"
#include "aqua/runtime/runtime_state.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace aqua::runtime {

struct ServerRuntimeConfig {
    audio::AudioFormat format;
    std::uint32_t frame_count = 0;
    std::string udp_bind_ip = "0.0.0.0";
    std::uint16_t udp_port = 0;
    std::chrono::milliseconds session_timeout { config::SESSION_TIMEOUT };
    std::chrono::milliseconds session_reap_interval { config::SESSION_REAP_INTERVAL };
    std::uint32_t network_queue_slots = config::SERVER_NETWORK_QUEUE_SLOTS;
    audio::AudioCaptureConfig capture;
    std::string rpc_bind_ip = "0.0.0.0";
    std::uint16_t rpc_port = 50051;
    std::string advertised_udp_address = "127.0.0.1";
};

class ServerRuntime final : public std::enable_shared_from_this<ServerRuntime> {
public:
    ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 生命周期操作是单属主的控制面操作：start() 与 stop() 不得并发执行；
    // stop() 本身幂等，可重复调用。ServerRuntime 必须由 std::shared_ptr 持有，
    // 因为 reap 定时器使用 weak_from_this()。
    bool start();
    void stop() noexcept;

    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_ ? sessions_->session_count() : 0; }
    [[nodiscard]] RuntimeState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] audio::AudioError last_audio_error() const noexcept
    {
        return last_audio_error_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept { return dispatcher_.frames_encoded(); }
    [[nodiscard]] std::uint64_t frames_broadcast() const noexcept { return dispatcher_.frames_broadcast(); }
    [[nodiscard]] std::uint64_t frames_without_clients() const noexcept
    {
        return dispatcher_.frames_without_clients();
    }
    [[nodiscard]] std::uint64_t encode_failures() const noexcept
    {
        return dispatcher_.encode_failures();
    }
    [[nodiscard]] std::uint64_t dispatch_failures() const noexcept
    {
        return dispatcher_.dispatch_failures();
    }
    [[nodiscard]] std::uint64_t udp_hello_received() const noexcept { return udp_.hello_received(); }
    [[nodiscard]] std::uint64_t udp_hello_rejected() const noexcept { return udp_.hello_rejected(); }
    [[nodiscard]] std::uint64_t udp_sessions_established() const noexcept { return udp_.sessions_established(); }
    [[nodiscard]] std::uint64_t udp_sessions_refreshed() const noexcept { return udp_.sessions_refreshed(); }
    [[nodiscard]] std::uint64_t udp_hello_ack_sent() const noexcept { return udp_.hello_ack_sent(); }
    [[nodiscard]] std::uint64_t udp_malformed_datagrams() const noexcept { return udp_.malformed_datagrams(); }
    [[nodiscard]] std::uint64_t udp_non_hello_datagrams() const noexcept { return udp_.non_hello_datagrams(); }
    [[nodiscard]] net::UdpTransportStats udp_stats() const noexcept
    {
        return udp_.stats();
    }
    [[nodiscard]] std::uint16_t udp_port() const noexcept
    {
        return udp_.local_endpoint().port();
    }
    [[nodiscard]] std::uint64_t frames_dropped_before_network() const noexcept
    {
        return dispatcher_.dropped_frames();
    }
    [[nodiscard]] std::uint64_t packetizer_rejected_unaligned_blocks() const noexcept
    {
        return packetizer_.rejected_unaligned_blocks();
    }
    [[nodiscard]] std::uint64_t queue_accepted_frames() const noexcept { return frame_queue_.accepted_frames(); }
    [[nodiscard]] std::uint64_t queue_consumed_frames() const noexcept { return frame_queue_.consumed_frames(); }
    [[nodiscard]] std::uint64_t queue_dropped_frames() const noexcept { return frame_queue_.dropped_frames(); }
    [[nodiscard]] std::uint32_t queue_depth() const noexcept { return frame_queue_.size_slots(); }
    [[nodiscard]] session::SessionManager::Stats session_stats() const noexcept { return sessions_->stats(); }
    [[nodiscard]] std::uint64_t packetizer_input_blocks() const noexcept { return packetizer_.input_blocks(); }
    [[nodiscard]] std::uint64_t packetizer_input_bytes() const noexcept { return packetizer_.input_bytes(); }
    [[nodiscard]] std::uint64_t packetizer_frames_emitted() const noexcept { return packetizer_.frames_emitted(); }
    [[nodiscard]] std::uint64_t dispatcher_published_frames() const noexcept { return dispatcher_.published_frames(); }
    [[nodiscard]] std::uint64_t dispatcher_worker_wakeups() const noexcept { return dispatcher_.worker_wakeups(); }

    [[nodiscard]] bool capture_running() const noexcept
    {
        return capture_ != nullptr && capture_->is_running();
    }

private:
    struct ReapState;

    void on_capture_block(const audio::AudioBlock& block) noexcept;
    void on_capture_event(audio::AudioError error) noexcept;
    static void schedule_reap(const std::shared_ptr<ReapState>& reap,
        const std::weak_ptr<ServerRuntime>& weak_self,
        std::chrono::milliseconds interval, std::chrono::milliseconds timeout);
    bool enter_starting() noexcept;
    bool enter_stopping() noexcept;
    void enter_stopped() noexcept;

    ServerRuntimeConfig config_;
    asio::io_context& ioc_;
    std::unique_ptr<audio::AudioDeviceManager> device_mgr_;
    std::unique_ptr<audio::AudioCapture> capture_;
    std::shared_ptr<session::SessionManager> sessions_;
    net::UdpServer udp_;
    audio::AudioPacketizer packetizer_;
    audio::AudioFrameQueue frame_queue_;
    AudioNetworkDispatcher dispatcher_;
    std::unique_ptr<grpc::GrpcServer> grpc_;
    std::thread grpc_thread_;
    struct ReapState {
        using Strand = asio::strand<asio::io_context::executor_type>;
        explicit ReapState(asio::io_context& ioc)
            : strand(asio::make_strand(ioc))
            , timer(std::make_shared<asio::steady_timer>(strand))
        {
        }
        Strand strand;
        std::shared_ptr<asio::steady_timer> timer;
    };
    std::shared_ptr<ReapState> reap_state_;
    std::atomic<RuntimeState> state_ { RuntimeState::Created };
    std::atomic<audio::AudioError> last_audio_error_ { audio::AudioError::None };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
