#ifndef AQUA_UDP_CONFIG_H
#define AQUA_UDP_CONFIG_H

// UDP 传输层（UdpServer / UdpClient / UdpSocketBase）的可调参数集中地。
// 所有常量位于 aqua::config 命名空间，与 grpc_config.h / audio_config.h 风格一致；
// 调整后重新编译即可生效，无需改动业务代码。

#include <cstddef>

namespace aqua::config {

// UDP 内核接收缓冲区大小（SO_RCVBUF，单位字节）。
// 同时作为用户态预分配接收缓冲大小。Aqua 音频 datagram 本身远小于该值；
// 较大的内核队列用于吸收短时间 scheduler/network burst，降低高负载下的 kernel drop。
inline constexpr std::size_t UDP_RECV_BUFFER_BYTES = 64 * 1024;

// UDP 内核发送缓冲区大小（SO_SNDBUF，单位字节）。
// 用于吸收短时间发送突发；应用层仍有独立的有界 datagram queue（见
// UDP_MAX_QUEUED_DATAGRAMS），两者职责不同，互不替代。
inline constexpr std::size_t UDP_SEND_BUFFER_BYTES = 64 * 1024;

// 用户态 transport 发送队列上限（按 datagram 个数）。
// 超限时丢弃最旧 datagram：实时音频场景下旧包比新包更没有价值，
// drop-oldest 能同时压低端到端延迟与内存占用，避免 scheduler stall 期间
// 形成无界 backlog。
inline constexpr std::size_t UDP_MAX_QUEUED_DATAGRAMS = 64;

} // namespace aqua::config

#endif // AQUA_UDP_CONFIG_H
