#ifndef AQUA_RUNTIME_SERVER_RUNTIME_H
#define AQUA_RUNTIME_SERVER_RUNTIME_H

// ServerRuntime：server 数据面编排（不拥有音频设备 / WASAPI realtime loop）。
//
// 职责：
//   - 持有 SessionManager（同时供 CLI 侧的 GrpcServer 复用做控制面）；
//   - UdpServer 收 HELLO → HelloResponder → establish_session；
//   - 采集回调把 PCM 交给 push_pcm() → AudioPacketizer 切成固定 F 帧 → encode → 广播。
//
// 采集（AudioCapture）与控制面（GrpcServer）由 CLI 拥有，分别经 push_pcm() / sessions()
// 与本类对接，避免 runtime 反向依赖平台 backend（见 doc/audio_design.md）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/net/udp/udp_server.h"
#include "aqua/session/hello.h"
#include "aqua/session/session_manager.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aqua::runtime {

struct ServerRuntimeConfig {
    audio::AudioFormat format;
    std::uint32_t frames_per_slot = 0; // F：每 AudioFrame 的 sample frame 数
    std::string udp_bind_ip = "0.0.0.0";
    std::uint16_t udp_port = 0;
    std::chrono::milliseconds session_timeout { 5000 }; // 会话超时（无 HELLO 则移除）
    std::chrono::milliseconds session_reap_interval { 1000 }; // 清理周期
};

class ServerRuntime final : public std::enable_shared_from_this<ServerRuntime> {
public:
    ServerRuntime(asio::io_context& ioc, const ServerRuntimeConfig& config);
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 绑定 UDP 并开始接收 HELLO；失败返回 false（transport 可重试或重建）。
    // 必须用 std::make_shared 创建本类（start() 内部经 shared_from_this 把收包
    // 回调与 runtime 生命周期绑定，避免 stop 后 transport State 仍持 handler 导致 UAF）。
    bool start();
    // 停止 UDP 收发（幂等）。调用后再析构，保证 receive 回调不再触发。
    void stop();

    // 采集入口（由 CLI 的 capture 回调调用，可在实时线程）：packetize → encode → 广播。
    void push_pcm(std::span<const std::byte> pcm) noexcept;

    // 收包入口（由 UdpServer 回调转调）：路由到 HELLO 处理。
    void handle_datagram(const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data) noexcept;

    SessionManager& sessions() noexcept { return sessions_; }
    [[nodiscard]] std::uint64_t frames_broadcast() const noexcept
    {
        return frames_broadcast_.load(std::memory_order_relaxed);
    }

private:
    static void on_packetized(void* ud, std::uint64_t sequence,
        std::span<const std::byte> pcm) noexcept;
    static void on_ack(void* ud, const asio::ip::udp::endpoint& target,
        std::span<const std::byte> ack) noexcept;

    void broadcast(std::shared_ptr<const std::vector<std::byte>> packet) noexcept;
    void schedule_reap();

    ServerRuntimeConfig config_;
    asio::io_context& ioc_;
    SessionManager sessions_;
    net::UdpServer udp_;
    HelloResponder hello_;
    audio::AudioPacketizer packetizer_;
    std::unique_ptr<asio::steady_timer> reap_timer_;
    std::atomic<std::uint64_t> frames_broadcast_ { 0 };
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_SERVER_RUNTIME_H
