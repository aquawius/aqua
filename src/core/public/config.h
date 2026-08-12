#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

#include <chrono>

namespace aqua::config {

// UDP session 超时：超过此时间未收到任何 UDP 包则标记 Expired
inline constexpr std::chrono::seconds UDP_SESSION_TIMEOUT{5};

// Server 扫描过期 session 的周期
inline constexpr std::chrono::seconds EXPIRED_CLEANUP_INTERVAL{2};

// Client 发送 UDP HELLO 保活的间隔。
// 单路保活：UDP HELLO 同时刷新 NAT 映射与 server session last_seen。
// 必须 < UDP_SESSION_TIMEOUT / 2，确保超时前至少有 2 次保活机会（5s timeout, 1s interval → 5 次机会）。
inline constexpr std::chrono::seconds KEEPALIVE_INTERVAL{1};

// Client 等待 UDP HELLO_ACK 的重试间隔
inline constexpr std::chrono::seconds HELLO_RETRY_INTERVAL{2};

// Client 无音频数据接收超时：超过此时间未收到任何 Audio 包则认为 server 已断开，
// 触发优雅退出。应 > 几个 HELLO 间隔以容忍网络抖动；与 UDP_SESSION_TIMEOUT 对齐
// （server 侧 session 5s 超时，client 侧 5s 无数据退出）。
inline constexpr std::chrono::seconds CLIENT_AUDIO_TIMEOUT{5};

// UDP 接收缓冲大小（覆盖最大 UDP datagram）
inline constexpr std::size_t UDP_RECV_BUF_SIZE = 65536;

// 每个音频包的时长（毫秒）
inline constexpr std::uint32_t AUDIO_PACKET_MS = 10;

// 音频采集 RingBuffer 大小
inline constexpr std::size_t CAPTURE_RINGBUFFER_SIZE = 64 * 1024;

// 音频播放 RingBuffer 大小
inline constexpr std::size_t PLAYBACK_RINGBUFFER_SIZE = 128 * 1024;

} // namespace aqua::config

#endif // AQUA_CONFIG_H
