#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

#include <chrono>

namespace aqua::config {

// UDP session 超时：超过此时间未收到任何 UDP 包则标记 Expired
inline constexpr std::chrono::seconds UDP_SESSION_TIMEOUT{5};

// Server 扫描过期 session 的周期
inline constexpr std::chrono::seconds EXPIRED_CLEANUP_INTERVAL{2};

// Client 发送 KeepAlive 的间隔
// 必须显著小于 UDP_SESSION_TIMEOUT，确保超时前至少有 2 次 KeepAlive 机会
inline constexpr std::chrono::seconds KEEPALIVE_INTERVAL{2};

// Client 等待 UDP HELLO_ACK 的重试间隔
inline constexpr std::chrono::seconds HELLO_RETRY_INTERVAL{2};

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
