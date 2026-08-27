#ifndef AQUA_RUNTIME_SERVER_RUNTIME_H
#define AQUA_RUNTIME_SERVER_RUNTIME_H

// ServerRuntime：server 侧的统领（唯一入口）。
//
// 职责（一次性装配 + 关停）：
//   - own 设备管理器 + 采集后端（经工厂，不依赖平台实现）+ SessionManager + UdpServer
//     + AudioPacketizer + AudioFrameQueue + AudioNetworkDispatcher + GrpcServer；
//   - 采集回调 → packetize → bounded RT→network handoff → dispatcher 编码广播；
//   - session 超时清理（reap 定时器）。
//
// 关停顺序固定为：capture → dispatcher(drain) → udp → gRPC，见 stop()。

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
#include <string>
#include <thread>

namespace aqua::runtime {

struct ServerRuntimeConfig {
    audio::AudioFormat format;
    std::uint32_t frame_count = 0; // F：每 AudioFrame 的 sample frame 数
    std::string udp_bind_ip = "0.0.0.0";
    std::uint16_t udp_port = 0;
    std::chrono::milliseconds session_timeout { config::SESSION_TIMEOUT };
    std::chrono::milliseconds session_reap_interval { config::SESSION_REAP_INTERVAL };
    std::uint32_t network_queue_slots = config::SERVER_NETWORK_QUEUE_SLOTS; // RT→network handoff only

    audio::AudioCaptureConfig capture;      // source / device / frames_per_buffer（format 由 runtime 内部与 format 对齐）
    std::string rpc_bind_ip = "0.0.0.0";
    std::uint16_t rpc_port = 50051;
    std::string advertised_udp_address = "127.0.0.1"; // 通告给 client 的 UDP 地址（端口取 udp_port）
};

class ServerRuntime final : public std::enable_shared_from_this<ServerRuntime> {
public:
    ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 一次性装配：设备 -> 采集后端 -> 数据面(udp+dispatcher+reap) -> gRPC -> 采集。失败即回滚并进入 Stopped。
    // 必须用 std::make_shared 创建（reap 定时器回调经 shared_from_this 与生命周期绑定）。
    bool start();
    // 幂等关停：capture -> dispatcher(drain) -> udp -> gRPC；停止后不可再次启动。
    void stop();

    session::SessionManager& sessions() noexcept { return *sessions_; }
    [[nodiscard]] RuntimeState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] audio::AudioError last_audio_error() const noexcept
    {
        return last_audio_error_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept { return dispatcher_.frames_encoded(); }
    [[nodiscard]] std::uint64_t frames_dropped_before_network() const noexcept
    {
        return dispatcher_.dropped_frames();
    }
    [[nodiscard]] std::uint64_t frames_broadcast() const noexcept
    {
        return dispatcher_.frames_broadcast();
    }
    [[nodiscard]] std::uint64_t frames_without_clients() const noexcept
    {
        return dispatcher_.frames_without_clients();
    }
    [[nodiscard]] bool capture_running() const noexcept
    {
        return capture_ != nullptr && capture_->is_running();
    }

private:
    void on_capture_block(const audio::AudioBlock& block) noexcept;
    void on_capture_event(audio::AudioError error) noexcept;
    void schedule_reap();

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
    std::unique_ptr<asio::steady_timer> reap_timer_;
    std::atomic<RuntimeState> state_ { RuntimeState::Created };
    std::atomic<audio::AudioError> last_audio_error_ { audio::AudioError::None };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
