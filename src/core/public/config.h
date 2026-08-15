#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

#include <chrono>

namespace aqua::config {

// Server 侧 session 超时：超过此时间未收到任何 UDP HELLO 则标记过期。
// last_seen 仅由 UDP HELLO 刷新（Audio 包不刷新）。
inline constexpr std::chrono::seconds SESSION_TIMEOUT { 5 };

// Server 扫描并清理过期 session 的周期。
inline constexpr std::chrono::seconds SESSION_CLEANUP_INTERVAL { 3 };

// Client 发送 UDP HELLO 保活的间隔。
// 单路保活：UDP HELLO 同时刷新 NAT 映射与 server session last_seen。
// 必须 < SESSION_TIMEOUT / 2，确保超时前至少有 2 次保活机会（5s timeout, 1s interval → 5 次机会）。
inline constexpr std::chrono::seconds HELLO_KEEPALIVE_INTERVAL { 1 };

// Client 握手阶段重发 HELLO 的间隔（上次 HELLO 无 ACK 后隔这么久再发）。
inline constexpr std::chrono::milliseconds HELLO_HANDSHAKE_RETRY_INTERVAL { 800 };

// Client 握手阶段最多发送 HELLO 的次数（含首次，即最大尝试次数）。
// HELLO_HANDSHAKE_RETRY_INTERVAL × HELLO_HANDSHAKE_MAX_ATTEMPTS = 800ms × 6 = ~5s。
inline constexpr int HELLO_HANDSHAKE_MAX_ATTEMPTS { 6 };

// Client 无音频数据接收超时：超过此时间未收到任何 Audio 包则认为 server 已断开，
// 触发重连（--auto-reconnect）或优雅退出。应 > 几个 HELLO 间隔以容忍网络抖动；
// 与 SESSION_TIMEOUT 对齐（server 侧 session 5s 超时，client 侧 5s 无数据退出）。
inline constexpr std::chrono::seconds CLIENT_AUDIO_RECV_TIMEOUT { 5 };

// 保活 HELLO 连续未收到 HELLO_ACK 的次数阈值：达到此值记 warn，
// 早于 CLIENT_AUDIO_RECV_TIMEOUT（5s）暴露服务器已断（3s @ HELLO_KEEPALIVE_INTERVAL=1s）。
inline constexpr std::uint32_t HELLO_ACK_WARN_THRESHOLD = 3;

// ---- 自动重连（--auto-reconnect）指数退避参数 ----
inline constexpr std::chrono::seconds RECONNECT_BASE_DELAY { 1 };
inline constexpr std::chrono::seconds RECONNECT_MAX_DELAY { 30 };
// 会话稳定运行超过此时长后，重连退避重置回基础值（避免长时间稳定后断线仍要等 30s）。
inline constexpr std::chrono::seconds RECONNECT_BACKOFF_RESET_AFTER { 30 };

// UDP 接收缓冲大小（字节），覆盖最大 UDP datagram。
inline constexpr std::size_t UDP_RECV_BUFFER_BYTES = 65536;

// 每个音频包的帧数。这是传输/打包参数，不属于 AudioFormat。
// 采用帧数（而非毫秒）作为基本单位：JB 时间线推进量 packet_duration =
// frames_per_packet × 10^6 / sample_rate 精确等于音频内容真实时长，任何采样率
// 都不会产生截断漂移（若用毫秒定义，44.1kHz 下 frames=132.3 被截断为 132，
// packet_duration=2993μs≠3000μs，每包漂移 7μs，~3s 累积即触发误 rebase）。
// 48kHz: 144 帧 = 3ms；payload = 144 × 8 = 1152B + 15B header = 1167B UDP < 1500 MTU。
// 44.1kHz: 144 帧 ≈ 3.27ms；96kHz: 144 帧 = 1.5ms（包率翻倍，但无漂移）。
inline constexpr std::uint32_t AUDIO_FRAMES_PER_PACKET = 144;

// RingBuffer 最小容量（字节）。仅作防御下限（配合下方 RINGBUFFER_ALIGNMENT_BYTES 对齐）。
// 实际最小容量由 RINGBUFFER_ALIGNMENT_BYTES 决定：任何 < 1024 的请求最终都会对齐到 1024。
inline constexpr std::size_t RINGBUFFER_MIN_BYTES = 64;

// RingBuffer 容量对齐粒度（字节）。SpscRingBuffer 构造时把请求容量向上取整为该值的倍数，
// 例如 8000 -> 8192、10000 -> 10240。相比 2 的幂取整（10000 -> 16384，+63%），
// 1KiB 对齐显著减少过度分配。容量只影响突发余量，不影响延迟（延迟由占用水位决定）。
inline constexpr std::size_t RINGBUFFER_ALIGNMENT_BYTES = 1024;

// 音频采集 RingBuffer 默认大小（字节），可被 --capture-buffer 覆盖。
// 48kHz/F32LE/2ch = 384000 B/s。WASAPI 共享模式约 10ms 交付一批 ≈ 3840 bytes。
// 8KB 可容纳 2 批 WASAPI 交付（7680 bytes），防止系统繁忙时 capture 线程
// 被延迟调度导致的数据丢失。实际稳态占用远低于容量。
inline constexpr std::size_t DEFAULT_CAPTURE_RINGBUFFER_BYTES = 8 * 1024;

// 音频播放 RingBuffer 默认大小（字节），可被 --playback-buffer 覆盖。
// 48kHz/F32LE/2ch = 384000 B/s。JB timer 批量 pop（Windows 定时器粒度 ~15.6ms，
// 每次可能 pop 5~6 包 = 5760~6912 bytes），WASAPI playback 约 10ms 读取一批 ≈ 3840 bytes。
// 峰值占用 = 6 × 1152 + WASAPI 间隙残留 ≈ 9000 bytes。
// 16KB > 9000，安全。容量不直接影响延迟（延迟由占用水位决定）。
inline constexpr std::size_t DEFAULT_PLAYBACK_RINGBUFFER_BYTES = 16 * 1024;

// JitterBuffer 默认目标延迟（毫秒），可被 CLI --jitter-latency 覆盖。
// 换算为包数后传入 JitterBuffer 构造函数。48kHz 下 30ms / (144帧/3ms) = 10 包。
// JitterBuffer 是唯一的主要网络缓冲。
inline constexpr std::uint32_t DEFAULT_JITTER_TARGET_LATENCY_MS = 30;

// JitterBuffer 漂移检测窗口大小（包数）。
// 每 JITTER_DRIFT_WINDOW_PACKETS 个包评估一次 late 比例，超过阈值则 rebase 时间线。
inline constexpr std::uint32_t JITTER_DRIFT_WINDOW_PACKETS = 1000;

// JitterBuffer 漂移检测 late 包数阈值（窗口内 late 包数）。
// 窗口内 late 包数 >= 此值时触发 rebase。15/1000 = 1.5%。
// 真实时钟漂移的 late rate 通常在 0.5-2%，1.5% 可在 ~1 分钟内捕获而稳定阶段 late=0 不误触。
inline constexpr std::uint32_t JITTER_DRIFT_LATE_PACKET_THRESHOLD = 15;

// ---- 运行时可配置参数 ----
// 前端（CLI / UI）填充此结构体后传入 core 组件构造函数。
// core 不依赖全局状态，所有可调参数通过此结构体注入。
struct RuntimeConfig {
    // JitterBuffer 目标延迟（毫秒）
    std::uint32_t jitter_target_latency_ms = DEFAULT_JITTER_TARGET_LATENCY_MS;

    // JitterBuffer 漂移检测窗口大小（包数）
    std::uint32_t jitter_drift_window_size = JITTER_DRIFT_WINDOW_PACKETS;

    // JitterBuffer 漂移检测 late 包数阈值
    std::uint32_t jitter_drift_late_threshold = JITTER_DRIFT_LATE_PACKET_THRESHOLD;

    // 播放 RingBuffer 大小（字节）
    std::size_t playback_ringbuffer_size = DEFAULT_PLAYBACK_RINGBUFFER_BYTES;

    // 采集 RingBuffer 大小（字节）
    std::size_t capture_ringbuffer_size = DEFAULT_CAPTURE_RINGBUFFER_BYTES;
};

} // namespace aqua::config

#endif // AQUA_CONFIG_H
