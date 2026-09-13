#ifndef AQUA_NET_UDP_UDP_CONFIG_H
#define AQUA_NET_UDP_UDP_CONFIG_H

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

// 单个 Audio payload 的字节上限。IPv6 不分片预算为
// 1500 - 40(IPv6 头) - 8(UDP 头) - 12(RTP 头) = 1440；在此之上主动让出 40 字节
// 给隧道/额外封装（GRE/IPIP 20B、PPPoE 8B、VPN/WireGuard 等）：走这类链路时 1440
// 仍可能越界分片，1400 是兼顾常见封装的保守取值。IPv4 预算（1460）自然同时满足。
inline constexpr std::size_t UDP_AUDIO_PAYLOAD_BYTES = 1400;

// auto-F 的包时长上限（ms）：F = min(MTU 预算帧数, floor(sample_rate × 本值))。
// 只按 MTU 预算推导会让包时长随格式漂移（48kHz：stereo F32 3.5ms / mono S16
// 14.6ms / mono U8 29.2ms），而 JB 的每槽粒度 = 包时长——低延迟链路要求各格式
// 的包时长处于同一量级。5ms 下：48kHz mono S16 F=240、44.1kHz stereo S16 F=220
//（MTU 预算 350 不封顶）；48kHz stereo F32 仍由 MTU 封顶（175 ≈ 3.65ms）。
inline constexpr double UDP_AUDIO_MAX_PACKET_MS = 5.0;

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
// client→server 只有一种包；server 对每个合法 heartbeat 都回 ACK
// （首包建连，之后是路径探活回执），client 两阶段都做 ACK 跟踪。
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT { 5000 };
inline constexpr std::chrono::milliseconds SESSION_REAP_INTERVAL { 1000 };
// 握手期节奏（association 未建立前；建立后同一包型转 HEARTBEAT_INTERVAL 节奏）。
inline constexpr std::chrono::milliseconds HEARTBEAT_HANDSHAKE_INTERVAL { 1000 };
// 握手期失败阈值：连续 3 个周期无 ACK 即建连失败（fail-fast，约 3s）。
inline constexpr std::uint32_t HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD = 3;
// 稳态节奏（1s：NAT 锥形映射通常 30s+ 超时，余量充足）与失败阈值
// （连续 5 个周期无 ACK 即 UDP 路径死亡，约 5s，与 SESSION_TIMEOUT 对齐，
// 双向死亡检测对称：server 5s 摘 session，client 5s 判路径死亡）。
// activity-aware：距上次 client→server 发包不足一周期则跳过（下游音频不抑制——
// 下行包维持不了上行 NAT 映射；跳过的周期不计 miss）。
inline constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL { 1000 };
inline constexpr std::uint32_t HEARTBEAT_ACK_MISS_THRESHOLD = 5;

} // namespace aqua::config

#endif // AQUA_NET_UDP_UDP_CONFIG_H
