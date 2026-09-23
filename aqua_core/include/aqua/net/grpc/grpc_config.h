#ifndef AQUA_NET_GRPC_GRPC_CONFIG_H
#define AQUA_NET_GRPC_GRPC_CONFIG_H

// gRPC 控制面（Connect / Disconnect）的可调参数。
// 常量位于 aqua::config 命名空间，与 udp_config.h / audio_config.h 风格一致。
// 注意：UDP 保活（Heartbeat）不经过 gRPC，gRPC 只负责 session 生命周期管理。

#include <chrono>
#include <cstddef>

namespace aqua::config {

// gRPC 控制面默认端口（server 监听 / client 连接）。
inline constexpr std::uint16_t DEFAULT_RPC_PORT = 50051;

// 默认 client 显示名（仅用于 server 侧识别与日志；长度上限见 GRPC_MAX_CLIENT_NAME_BYTES）。
inline constexpr char DEFAULT_CLIENT_NAME[] = "aqua-client";

// Connect RPC 与 connect_to_server 的等待/调用超时。
// server TCP 已连但 RPC 线程卡死时，防止客户端无限阻塞。
inline constexpr std::chrono::milliseconds GRPC_CONNECT_DEADLINE { 3000 };

inline constexpr std::size_t GRPC_MAX_CLIENT_NAME_BYTES { 128 };

// Disconnect RPC 超时。
// server 可能已崩溃，同步调用默认的重试会阻塞约 2s；局域网内 1s 足够完成
// RPC，超时则放弃（best-effort 清理，不阻塞 client 退出）。
inline constexpr std::chrono::milliseconds GRPC_DISCONNECT_DEADLINE { 1000 };

// proto Keepalive 探活节奏（应用层行为，与 gRPC 版本无关）：client 每间隔一次
// 带 deadline 的 Keepalive RPC。传输层 channel 参数保持默认——存活判定只看
// 应用层结果，从根上杜绝 GOAWAY 误杀那类版本相关调参事故。
// 1s 间隔 / 800ms 单次超时：deadline 必须小于 interval，否则慢 ping 会和下
// 一个周期重叠堆积；800ms 相对局域网 RTT 有两个数量级余量。
// session 超时（SESSION_TIMEOUT = 5s）是间隔的 5 倍，单个 ping 抖动不误杀。
inline constexpr std::chrono::milliseconds GRPC_KEEPALIVE_INTERVAL { 1000 };
inline constexpr std::chrono::milliseconds GRPC_KEEPALIVE_DEADLINE { 800 };
// 传输失败容忍：连续 N 次 Keepalive RPC 失败才判 TransportDead（与 UDP 稳态
// HEARTBEAT_ACK_MISS_THRESHOLD = 5、SESSION_TIMEOUT = 5s 对齐：单次 800ms
// 毛刺不杀 client）。SessionGone（server 明确说会话不在）仍立即上报——
// 那是确定性结论，不是抖动。
inline constexpr std::uint32_t GRPC_KEEPALIVE_MISS_THRESHOLD = 5;

} // namespace aqua::config

#endif // AQUA_NET_GRPC_GRPC_CONFIG_H
