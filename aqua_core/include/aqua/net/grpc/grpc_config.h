#ifndef AQUA_GRPC_CONFIG_H
#define AQUA_GRPC_CONFIG_H

// gRPC 控制面（Connect / Disconnect）的可调参数。
// 常量位于 aqua::config 命名空间，与 udp_config.h / audio_config.h 风格一致。
// 注意：UDP 保活（HELLO）不经过 gRPC，gRPC 只负责 session 生命周期管理。

#include <chrono>
#include <cstddef>

namespace aqua::config {

// Connect RPC 与 connect_to_server 的等待/调用超时。
// server TCP 已连但 RPC 线程卡死时，防止客户端无限阻塞。
inline constexpr std::chrono::milliseconds GRPC_CONNECT_DEADLINE { 3000 };

inline constexpr std::size_t GRPC_MAX_CLIENT_NAME_BYTES { 128 };

// Disconnect RPC 超时。
// server 可能已崩溃，同步调用默认的重试会阻塞约 2s；局域网内 1s 足够完成
// RPC，超时则放弃（best-effort 清理，不阻塞 client 退出）。
inline constexpr std::chrono::milliseconds GRPC_DISCONNECT_DEADLINE { 1000 };

} // namespace aqua::config

#endif // AQUA_GRPC_CONFIG_H
