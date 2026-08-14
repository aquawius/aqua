#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

#include <chrono>

namespace aqua::config {

// UDP session 超时：超过此时间未收到任何 UDP 包则标记 Expired
inline constexpr std::chrono::seconds UDP_SESSION_TIMEOUT { 5 };

// Server 扫描过期 session 的周期
inline constexpr std::chrono::seconds EXPIRED_CLEANUP_INTERVAL { 3 };

// Client 发送 UDP HELLO 保活的间隔。
// 单路保活：UDP HELLO 同时刷新 NAT 映射与 server session last_seen。
// 必须 < UDP_SESSION_TIMEOUT / 2，确保超时前至少有 2 次保活机会（3s timeout, 1s interval → 3 次机会）。
inline constexpr std::chrono::seconds KEEPALIVE_INTERVAL { 1 };

// Client 等待 UDP HELLO_ACK 的重试间隔
inline constexpr std::chrono::milliseconds HELLO_RETRY_INTERVAL { 800 };

// Client 等待 UDP HELLO_ACK 的最大重试次数 HELLO_RETRY_INTERVAL × HELLO_MAX_RETRIES = 800ms × 6 = ~5s。
inline constexpr int HELLO_MAX_RETRIES { 6 };

// Client 无音频数据接收超时：超过此时间未收到任何 Audio 包则认为 server 已断开，
// 触发优雅退出。应 > 几个 HELLO 间隔以容忍网络抖动；与 UDP_SESSION_TIMEOUT 对齐
// （server 侧 session 5s 超时，client 侧 5s 无数据退出）。
inline constexpr std::chrono::seconds CLIENT_AUDIO_TIMEOUT { 5 };

// UDP 接收缓冲大小（覆盖最大 UDP datagram）
inline constexpr std::size_t UDP_RECV_BUF_SIZE = 65536;

// 每个音频包的时长（毫秒）。
// 这是传输/打包参数，不属于 AudioFormat。
// 3ms × 48kHz = 144 frames × 8 bytes/frame = 1152B payload + 15B header = 1167B UDP
// 完全落在以太网 1500 MTU 以内，避免 IP 分片。
inline constexpr std::uint32_t AUDIO_PACKET_MS = 3;

// 音频采集 RingBuffer 大小
// 48kHz/F32LE/2ch = 384000 B/s。WASAPI 共享模式约 10ms 交付一批 ≈ 3840 bytes。
// 8KB 可容纳 2 批 WASAPI 交付（7680 bytes），防止系统繁忙时 capture 线程
// 被延迟调度导致的数据丢失。实际稳态占用远低于容量。
inline constexpr std::size_t CAPTURE_RINGBUFFER_SIZE = 8 * 1024;

// 音频播放 RingBuffer 大小
// 48kHz/F32LE/2ch = 384000 B/s。JB timer 批量 pop（Windows 定时器粒度 ~15.6ms，
// 每次可能 pop 5~6 包 = 5760~6912 bytes），WASAPI playback 约 10ms 读取一批 ≈ 3840 bytes。
// 峰值占用 = 6 × 1152 + WASAPI 间隙残留 ≈ 9000 bytes。
// 16KB > 9000，安全。容量不直接影响延迟（延迟由占用水位决定）。
inline constexpr std::size_t PLAYBACK_RINGBUFFER_SIZE = 16 * 1024;

// JitterBuffer 默认目标延迟（毫秒）。仅作文档参考，
// 实际由 CLI --jitter-latency 动态配置，换算为包数后传入 JitterBuffer 构造函数。
// 30ms / 3ms(AUDIO_PACKET_MS) = 10 包。JitterBuffer 是唯一的主要网络缓冲。
inline constexpr std::uint32_t JITTER_TARGET_LATENCY_MS = 30;

// JitterBuffer 漂移检测窗口大小（包数）。
// 每 JITTER_DRIFT_WINDOW_SIZE 个包评估一次 late 比例，超过阈值则 rebase 时间线。
inline constexpr std::uint32_t JITTER_DRIFT_WINDOW_SIZE = 1000;

// JitterBuffer 漂移检测 late 包比例阈值。
// 窗口内 late 包数 >= 此值时触发 rebase。15/1000 = 1.5%。
// 真实时钟漂移的 late rate 通常在 0.5-2%，1.5% 可在 ~1 分钟内捕获而稳定阶段 late=0 不误触。
inline constexpr std::uint32_t JITTER_DRIFT_LATE_THRESHOLD = 15;

// ---- 运行时可配置参数 ----
// 前端（CLI / UI）填充此结构体后传入 core 组件构造函数。
// core 不依赖全局状态，所有可调参数通过此结构体注入。
struct RuntimeConfig {
    // JitterBuffer 目标延迟（毫秒）
    std::uint32_t jitter_target_latency_ms = JITTER_TARGET_LATENCY_MS;

    // JitterBuffer 漂移检测窗口大小（包数）
    std::uint32_t jitter_drift_window_size = JITTER_DRIFT_WINDOW_SIZE;

    // JitterBuffer 漂移检测 late 包比例阈值
    std::uint32_t jitter_drift_late_threshold = JITTER_DRIFT_LATE_THRESHOLD;

    // 播放 RingBuffer 大小（字节）
    std::size_t playback_ringbuffer_size = PLAYBACK_RINGBUFFER_SIZE;

    // 采集 RingBuffer 大小（字节）
    std::size_t capture_ringbuffer_size = CAPTURE_RINGBUFFER_SIZE;
};

} // namespace aqua::config

#endif // AQUA_CONFIG_H
