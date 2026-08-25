#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 数据面编排（不拥有音频回放设备）。
//
// 职责：
//   - gRPC Connect 拿 session_id / UDP endpoint / format / frames_per_slot；
//   - 据此建 JitterBuffer + AudioDepacketizer，UdpClient 收 Audio → 解包 → JB；
//   - 周期发 HELLO（NAT 保活）；
//   - 回放后端经 pull_playback() 从 JB 取数（AudioPlayback 由 CLI 拥有）。
//
// 回放设备（AudioPlayback）由 CLI 拥有，经 pull_playback() 与本类对接，避免 runtime
// 反向依赖平台 backend（见 doc/audio_design.md）。

#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/depacketizer/audio_depacketizer.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/session/hello.h"

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

class ClientRuntime : public std::enable_shared_from_this<ClientRuntime> {
public:
    ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // 控制面：连 gRPC，成功后建 JB / 解包器 / 设置 UDP 远端。
    bool connect(const std::string& server_ip, std::uint16_t rpc_port,
        const std::string& client_name);

    // 建 JB + 解包器（connect 内部调用；测试可绕过 gRPC 直接调用）。成功返回 true。
    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frames_per_slot);

    // 收包入口（由 UdpClient 回调转调）：Audio → 解包器 → JB；其他类型忽略。
    void handle_datagram(const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data) noexcept;

    // 回放入口（由 CLI 的 playback 回调调用）：从 JB 取数，返回填充帧数。
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;

    // 启动数据面（UDP 接收 + 周期 HELLO）；需先 connect 成功。
    // 必须用 std::make_shared 创建本类（start() 内部经 shared_from_this 把回调与
    // runtime 生命周期绑定，避免 stop 后 transport State 仍持 handler 导致 UAF）。
    bool start();
    void stop();

    const grpc::ConnectResult& connect_result() const noexcept { return connect_result_; }
    audio::JitterBuffer* jitter_buffer() noexcept { return jb_.get(); }

private:
    static void on_send_hello(void* ud, std::span<const std::byte> packet) noexcept;
    void schedule_hello();

    ClientRuntimeConfig config_;
    asio::io_context& ioc_;
    grpc::GrpcClient grpc_;
    net::UdpClient udp_;
    std::unique_ptr<audio::JitterBuffer> jb_;
    std::unique_ptr<audio::AudioDepacketizer> depacketizer_;
    std::unique_ptr<HelloSender> hello_sender_;
    std::unique_ptr<asio::steady_timer> hello_timer_;
    grpc::ConnectResult connect_result_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
