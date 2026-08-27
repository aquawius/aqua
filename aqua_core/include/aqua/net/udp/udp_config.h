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

// 用户态 transport pending 发送队列上限（按 datagram 个数）。
// 当前策略为 drop-oldest；in-flight datagram 独立持有，永远不会被溢出策略移除。
inline constexpr std::size_t UDP_MAX_QUEUED_DATAGRAMS = 64;

// Capture RT -> network worker handoff capacity. At the current 3 ms AudioFrame
// cadence, 4 slots cap this non-playout queue at roughly 12 ms of audio.
inline constexpr std::uint32_t SERVER_NETWORK_QUEUE_SLOTS = 5;

// ---- session 保活（UDP HELLO）与超时 ----
// HELLO_INTERVAL 必须远小于 SESSION_TIMEOUT：server 以 last_seen 超时清理 session
// （ServerRuntime::schedule_reap），若 HELLO 间隔接近超时，正常客户端会被误清。
// 二者放一起，让"改一个必须想到另一个"在物理上相邻。
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT { 5000 };
inline constexpr std::chrono::milliseconds SESSION_REAP_INTERVAL { 1000 };
inline constexpr std::chrono::milliseconds HELLO_INTERVAL { 1000 };

} // namespace aqua::config

#endif // AQUA_UDP_CONFIG_H
