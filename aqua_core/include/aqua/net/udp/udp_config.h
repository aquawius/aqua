#ifndef AQUA_NET_UDP_UDP_CONFIG_H
#define AQUA_NET_UDP_UDP_CONFIG_H

// UDP 传输层（UdpTransport）的可调参数集中地。
// 所有常量位于 aqua::config 命名空间，与 grpc_config.h / audio_config.h 风格一致；
// 调整后重新编译即可生效，无需改动业务代码。

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aqua::config {

// UDP 数据面默认监听端口（server 绑定；实际值由 server 通过 gRPC 下发给 client）。
inline constexpr std::uint16_t DEFAULT_UDP_PORT = 50000;

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

// 协议允许的**最大单个 datagram**（RTP 头 + 满payload），也就是 decode 侧认可
// 的音频上界。超过它一律判 malformed。
//
// 为什么独立于 UDP_RECV_BUFFER_BYTES：后者是**内核队列容量**（一批 datagram 的
// 排队空间），前者是**单个 datagram 的协议上界**。两者是不同的关注点，所以解码侧
// 与静态断言引用这一个。
//
// **注意别把它当成接收缓冲的尺寸**：接收缓冲必须覆盖"对端可能发来的最大值"
// （UDP 最大载荷 65507），否则 Windows 的 WSARecvFrom 会回WSAEMSGSIZE，而那会
// 计入 rx_errors 并驱动按族共享的接收退避。理由见 udp_transport.h 的 RECV_BUF_SIZE。
inline constexpr std::size_t RTP_HEADER_BYTES = 12;
inline constexpr std::size_t UDP_MAX_DATAGRAM_BYTES = RTP_HEADER_BYTES + UDP_AUDIO_PAYLOAD_BYTES;

// auto-F 的包时长上限（ms）：F = min(MTU 预算帧数, floor(sample_rate × 本值))。
// 只按 MTU 预算推导会让包时长随格式漂移（48kHz：stereo F32 3.5ms / mono S16
// 14.6ms / mono U8 29.2ms），而 JB 的每槽粒度 = 包时长——低延迟链路要求各格式
// 的包时长处于同一量级。5ms 下：48kHz mono S16 F=240、44.1kHz stereo S16 F=220
// （MTU 预算 350 不封顶）；48kHz stereo F32 仍由 MTU 封顶（175 ≈ 3.65ms）。
inline constexpr double UDP_AUDIO_MAX_PACKET_MS = 5.0;

// auto-F 的包时长下限（ms）：包时长 = F / sample_rate。极端格式（多声道 + 高
// 位深 + 高采样率）下 frame_bytes 很大，MTU 预算只能给出很小的 F —— 例如
// 7.1ch F32 @192kHz：frame_bytes=32 → F=43 → 0.224ms/包 ≈ 4464 包/s。此时
// pacing 间隔低于可实现精度（等效退化成突发），且"交接队列 16 槽"只等于 3.6ms
// 音频，而采集每 10ms 交付 ~45 包 —— 队列必然持续溢出丢帧。这类格式在 1400B
// payload 下根本无法可靠传输（要满足 1ms/包需要 F≥192 → 6144B 超限），因此
// 启动期直接拒绝，而不是静默跑起来疯狂丢帧。0.5ms（≤2000 包/s）允许到
// 8ch F32 @48kHz（0.896ms）与 stereo F32 @192kHz（0.91ms）。
inline constexpr double UDP_AUDIO_MIN_PACKET_MS = 0.5;

// 显式指定 F（每包 sample frame 数）时的下限：再小则 RTP 头开销占比过高。
inline constexpr std::uint32_t MIN_FRAMES_PER_SLOT = 16;

// 用户态 transport pending 发送队列上限（按 datagram 个数）。
// 当前策略为 drop-oldest；in-flight datagram 独立持有，永远不会被溢出策略移除。
// executor 调度失败时 pending 队列也会明确丢弃，避免形成永久不进展的半死队列。
inline constexpr std::size_t UDP_MAX_QUEUED_DATAGRAMS = 64;


// ---- session 存活（UDP heartbeat 建连续命）与超时 ----
// 存活模型（分层）：
//   proto Keepalive 负责 session/控制面存活（刷 last_seen，reaper 只看它）；
//   UDP heartbeat 负责 association 建立（首包）与 UDP 路径存活
//   （NAT 映射 + server 端 endpoint 续命；不碰 last_seen）。
// client→server 只有一种包；server 对每个合法 heartbeat 都回 ACK
// （首包建连，之后是路径探活回执），client 两阶段都做 ACK 跟踪。
// 本值是 reaper 的**判定**门限，必须 >= 客户端判死所需时间，否则会出现
// "服务端先把还活着的 session 摘掉、客户端随后拿到 session_valid=false"。
//
// 推导（此前写的是"间隔的 5 倍"，那是错的——间隔不是1s）：keepalive 线程**先睡满
// interval 再发 RPC**，所以真实周期 = interval + RPC 耗时，最坏挂满 deadline：
//     (1000 + 800) × 5 = 9000 ms
// 也就是说本值 5000 小于客户端判死所需的最坏窗口。这**不会**误杀健康客户端：
// 服务端摘 session 的前提是"5s 内没成功处理任何 Keepalive"，此时客户端的 RPC 同样
// 必然失败、也会独立判死，两边只是**检测时刻不同**（server 5.0~5.2s / client 5.0~9.0s），
// 不存在"只有服务端判死"的单边误杀。真风险是检测慢，不是误杀。
// 若要收紧检测速度：把 GRPC_KEEPALIVE_DEADLINE 调小（LAN RTT ~0.2ms，800ms 买到
// 的是检测延迟而不是容错余量），或订阅 gRPC connectivity state做快路径。
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT { 5000 };
// reaper 扫描周期。它直接叠加在上面的判定门限之上（最坏多等这么久才真正删除），
// 所以它纯粹是拆除延迟：扫描是 O(sessions) 持锁，200ms 与 1000ms 的CPU 成本都
// 可忽略，但能把"客户端被杀 → 服务端停止向它单播"的窗口从 5.0~6.0s 压到 5.0~5.2s。
inline constexpr std::chrono::milliseconds SESSION_REAP_INTERVAL { 200 };
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
