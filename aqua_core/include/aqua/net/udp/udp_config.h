#ifndef AQUA_UDP_CONFIG_H
#define AQUA_UDP_CONFIG_H

// UDP 传输层（UdpTransport）的可调参数集中地。
// 所有常量位于 aqua::config 命名空间，与 grpc_config.h / audio_config.h 风格一致；
// 调整后重新编译即可生效，无需改动业务代码。

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aqua::config {

// UDP 内核接收缓冲区大小（SO_RCVBUF，单位字节）。
// 同时作为用户态预分配接收缓冲大小。Aqua 音频 datagram 本身远小于该值；
// 较大的内核队列用于吸收短时间 scheduler/network burst，降低高负载下的 kernel drop。
inline constexpr std::size_t UDP_RECV_BUFFER_BYTES = 64 * 1024;

// UDP 内核发送缓冲区大小（SO_SNDBUF，单位字节）。
// 用于吸收短时间发送突发；应用层仍有独立的有界 datagram queue（见
// UDP_MAX_QUEUED_DATAGRAMS），两者职责不同，互不替代。
inline constexpr std::size_t UDP_SEND_BUFFER_BYTES = 64 * 1024;

// 在保守的 1500 字节以太网 MTU 内、IPv6 不分片的前提下，能容纳的最大 Audio payload：
// 1500 - 40(IPv6 头) - 8(UDP 头) - 12(RTP 头)。
inline constexpr std::size_t UDP_AUDIO_PAYLOAD_BYTES = 1440;

// 用户态 transport pending 发送队列上限（按 datagram 个数）。
// 当前策略为 drop-oldest；in-flight datagram 独立持有，永远不会被溢出策略移除。
// executor 调度失败时 pending 队列也会明确丢弃，避免形成永久不进展的半死队列。
inline constexpr std::size_t UDP_MAX_QUEUED_DATAGRAMS = 64;

// Capture RT -> network worker 交接队列容量。按当前 3 ms 的 AudioFrame 节奏，
// 4 个槽把这个非回放队列的音频量上限压在约 12 ms。

// ---- session 存活（UDP heartbeat 建连续命）与超时 ----
// 存活模型（分层）：
//   proto Keepalive 负责 session/控制面存活（刷 last_seen，reaper 只看它）；
//   UDP heartbeat 负责 association 建立（首包）与 UDP 路径存活
//   （NAT 映射 + server 端 endpoint 续命；不碰 last_seen）。
// client→server 只有一种包；server 按 session 状态区分：首包建连并回 ACK，
// 之后只刷新（无 ACK）。
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT { 30000 };
inline constexpr std::chrono::milliseconds SESSION_REAP_INTERVAL { 1000 };
// 握手期节奏（association 未建立前；建立后同一包型转 5s 续命节奏）。
inline constexpr std::chrono::milliseconds HELLO_INTERVAL { 1000 };
inline constexpr std::uint32_t HELLO_ACK_MISS_THRESHOLD = 3;
// 续命节奏（NAT 锥形映射通常 30s+ 超时，5s 有充分余量）。
// activity-aware：距上次 client→server 发包不足一周期则跳过（下游音频不抑制——
// 下行包维持不了上行 NAT 映射）。
inline constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL { 5000 };

} // namespace aqua::config

#endif // AQUA_UDP_CONFIG_H
