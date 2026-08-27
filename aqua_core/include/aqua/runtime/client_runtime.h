#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 侧的统领（唯一入口）。
//
// 职责（一次性装配 + 关停）：
//   - own 设备管理器 + 回放后端（经工厂）+ GrpcClient + UdpClient + JitterBuffer；
//   - gRPC Connect 拿 session/format → 建 JB → UDP 接收 Audio 帧 → JB → 回放后端 pull；
//   - 周期 HELLO 保活；stop 时 best-effort gRPC Disconnect。
//
// 回放格式以 server 下发的权威格式为准，客户端不做转换。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_config.h"

#include <asio.hpp>

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
    audio::AudioPlaybackConfig playback; // device / frames_per_buffer（format 由 Connect 结果决定）
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

    // 一次性装配：设备 -> 回放后端 -> connect(控制面) -> 数据面 -> 回放。失败即回滚。
    bool start();
    // 幂等关停：playback(join 音频线程) -> udp -> best-effort Disconnect；停止后不可再次启动。
    void stop();

    const grpc::ConnectResult& connect_result() const noexcept { return connect_result_; }
    audio::JitterBuffer* jitter_buffer() noexcept { return jb_.get(); }
    [[nodiscard]] bool playback_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

private:
    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frame_count);
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;

    ClientRuntimeConfig config_;
    asio::io_context& ioc_;
    std::unique_ptr<audio::AudioDeviceManager> device_mgr_;
    std::unique_ptr<audio::AudioPlayback> playback_;
    grpc::GrpcClient grpc_;
    net::UdpClient udp_;
    std::shared_ptr<audio::JitterBuffer> jb_; // 共享给 UdpClient 收包回调
    std::uint32_t frame_count_ = 0; // F
    std::uint32_t frame_bytes_ = 0; // bytes per sample frame
    grpc::ConnectResult connect_result_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
