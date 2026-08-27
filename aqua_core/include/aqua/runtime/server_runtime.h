#ifndef AQUA_RUNTIME_SERVER_RUNTIME_H
#define AQUA_RUNTIME_SERVER_RUNTIME_H

// ServerRuntime：server 数据面编排（不拥有音频设备 / WASAPI realtime loop）。
//
// 职责：
//   - 持有 SessionManager（同时供 CLI 侧的 GrpcServer 复用做控制面）；
//   - UdpServer 收 HELLO → establish_session + 回 Ack（协议层内部完成）；
//   - 采集回调把 PCM 交给 push_pcm() → AudioPacketizer 切成固定 F 帧 →
//     UdpServer::send_audio()（encode + 广播，协议层内部完成）；
//   - session 超时清理策略（reap 定时器）。
//
// 采集（AudioCapture）与控制面（GrpcServer）由 CLI 拥有，分别经 push_pcm() / sessions()
// 与本类对接，避免 runtime 反向依赖平台 backend（见 doc/audio_design.md）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace aqua::runtime {

struct ServerRuntimeConfig {
    audio::AudioFormat format;
    std::uint32_t frame_count = 0; // F：每 AudioFrame 的 sample frame 数
    std::string udp_bind_ip = "0.0.0.0";
    std::uint16_t udp_port = 0;
    std::chrono::milliseconds session_timeout { config::SESSION_TIMEOUT }; // 会话超时（无 HELLO 则移除）
    std::chrono::milliseconds session_reap_interval { config::SESSION_REAP_INTERVAL }; // 清理周期
};

class ServerRuntime final : public std::enable_shared_from_this<ServerRuntime> {
public:
    ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 绑定 UDP + 启动 HELLO 接收 + session 超时清理定时器；失败返回 false。
    // 必须用 std::make_shared 创建本类（reap 定时器回调经 shared_from_this 与
    // runtime 生命周期绑定，避免 stop 后回调悬垂导致 UAF）。
    bool start();
    // 停止 UDP 收发与清理定时器（幂等）。调用后再析构。
    void stop();

    // 采集入口（由 CLI 的 capture 回调调用，可在实时线程）：packetize → 广播。
    void push_pcm(std::span<const std::byte> pcm) noexcept;

    session::SessionManager& sessions() noexcept { return *sessions_; }
    [[nodiscard]] std::uint64_t frames_encoded() const noexcept
    {
        return udp_.frames_encoded();
    }

private:
    void schedule_reap();

    ServerRuntimeConfig config_;
    asio::io_context& ioc_;
    std::shared_ptr<session::SessionManager> sessions_; // 共享给 UdpServer 的收包 handler
    net::UdpServer udp_;
    audio::AudioPacketizer packetizer_;
    audio::AudioPacketizer::FrameHandler packetize_handler_; // 打包回调（捕获 this，构造期绑定一次）
    std::unique_ptr<asio::steady_timer> reap_timer_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
