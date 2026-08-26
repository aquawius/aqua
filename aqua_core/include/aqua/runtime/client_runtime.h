#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 数据面编排（不拥有音频回放设备）。
//
// 职责：
//   - gRPC Connect 拿 session_id / UDP endpoint / format / frames_per_slot；
//   - 据此建 JitterBuffer，UdpClient 收 Audio → 回调 AudioFrame → JB；
//   - 周期 HELLO 保活由 UdpClient 内部定时器完成；
//   - 回放后端经 pull_playback() 从 JB 取数（AudioPlayback 由 CLI 拥有）。
//
// 回放设备（AudioPlayback）由 CLI 拥有，经 pull_playback() 与本类对接，避免 runtime
// 反向依赖平台 backend（见 doc/audio_design.md）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"

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
    std::chrono::milliseconds hello_interval { 1000 };
};

class ClientRuntime final {
public:
    ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // 控制面：连 gRPC，成功后建 JB / 设置 UDP 远端。
    bool connect(const std::string& server_ip, std::uint16_t rpc_port,
        const std::string& client_name);

    // 建 JB（connect 内部调用；测试可绕过 gRPC 直接调用）。成功返回 true。
    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frames_per_slot);

    // 回放入口（由 CLI 的 playback 回调调用）：从 JB 取数，返回填充帧数。
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;

    // 启动数据面：UDP 接收（AudioFrame → JB）+ 周期 HELLO（已 connect 时）。
    // 未 connect 时仅启动接收（本地数据面验证路径）。
    // 须先 setup_playback 成功，失败返回 false。
    bool start();
    void stop();

    const grpc::ConnectResult& connect_result() const noexcept { return connect_result_; }
    audio::JitterBuffer* jitter_buffer() noexcept { return jb_.get(); }
    // UDP 本地 endpoint（临时端口；bind 后由 OS 分配）。
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const noexcept
    {
        return udp_.local_endpoint();
    }

private:
    ClientRuntimeConfig config_;
    asio::io_context& ioc_;
    grpc::GrpcClient grpc_;
    net::UdpClient udp_;
    std::shared_ptr<audio::JitterBuffer> jb_; // 共享给 UdpClient 收包回调
    std::uint32_t frames_per_slot_ = 0; // F（setup_playback 记录）
    grpc::ConnectResult connect_result_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
