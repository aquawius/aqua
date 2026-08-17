#ifndef AQUA_CONFIG_H
#define AQUA_CONFIG_H

#include <chrono>

namespace aqua::config {

// 库版本号（单一来源）。CLI --version 与 C API aqua_version() 共用，避免多处硬编码漂移。
inline constexpr const char* AQUA_VERSION = "0.0.1";

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

// 客户端诊断刷新间隔：collect_and_log（周期日志）与诊断快照缓存
//（ClientRuntime::diagnostics() 返回的数据）的更新频率。
// 快照语义：diagnostics() 任意时刻调用都立即返回最近一次快照（mutex 拷贝，
// 不阻塞、不触发采集），本常量只决定数据的新鲜度——外部轮询频率高于此值
//（如 Android UI 250ms）只会重复读到同一份快照。
inline constexpr std::chrono::milliseconds DIAGNOSTICS_REFRESH_INTERVAL { 3000 };

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

// ---- JitterBuffer 容量（用户面唯一 JB 参数）----
// --jitter-buffer <ms>：抖动缓冲总量预算。内部从 capacity 自动推导全部运行点：
//   capacity = bit_ceil(max(MIN_CAPACITY, ceil(ms→packets)))   2 的幂 ring
//   ceiling  = capacity / 2   自适应上限（上半区留乱序余量，契约不变）
//   floor    = capacity / 4   起播点兼自适应下限，AIMD 区间 [cap/4, cap/2]
// 单参数消除 floor/ceiling 匹配错误与区间塌缩两类配置问题；
// 默认 30ms @48kHz → cap 16 包(48ms), floor 4 包(12ms), ceiling 8 包(24ms)。
inline constexpr std::uint32_t DEFAULT_JITTER_BUFFER_MS = 30;

// capacity 下限（包）：2 的幂；保证 floor=cap/4 >= 2、ceiling=cap/2 >= 4，
// 自适应区间 [2, 4] 最小但有效。
inline constexpr std::uint32_t JITTER_MIN_CAPACITY_PACKETS = 8;

// 断流 reset 的最小"落后"阈值（毫秒）。pop_next 的 reset 阈值取
// max(target 缓冲时长, 此值)：调度器（steady_timer）在 Windows 定时器粒度
// ~15.6ms 下批量唤醒，唤醒时 deadline 合法落后可达一个定时器周期；
// target 小于 ~16ms 时若阈值 = target 时长，正常批量唤醒即被误判为断流，
// 产生 reset 风暴（实测小 target 配置 7 秒内 9 次）。20ms = 粒度 + 调度余量。
inline constexpr std::chrono::milliseconds JITTER_MIN_RESET_LATENESS_MS { 20 };

// 检测窗口（包数）：统计有效到达包（排除重复/畸形）中的 late 数，
// 窗口满时共用同一份计数评估——先 drift rebase 判定，再 AIMD 判定。
// 48kHz/3ms 每包下 500 包 ≈ 1.5s。纯内部参数（不暴露 CLI/UI）。
inline constexpr std::uint32_t JITTER_DETECT_WINDOW_PACKETS = 500;

// drift rebase：窗口内 late >= 此值 → 时间线 rebase（两端时钟速率失步的终态纠正）。
// 8/500 = 1.6%，与旧 15/1000 同比例（窗口统一时按比例缩放）。
// 样本减半方差增大：突发 WiFi 抖动误触 rebase 的概率略升，实机观察
// "clock drift detected" 日志频率，频繁则上调至 12（2.4%）。
inline constexpr std::uint32_t JITTER_DRIFT_REBASE_LATE_COUNT = 8;

// ---- JitterBuffer 自适应 target（慢速 AIMD）----
// floor 是下限兼初始值；late 压力下自动抬升，持续干净后缓慢回落，
// 区间 [floor, ceiling]。抬升/回落通过 next_deadline_ ± 1 个 packet_duration
// 实现（排水/蓄水各 1 拍），速率天然被下游 RB 调度限速（RB 满时排水暂停）。
// 评估复用检测窗口（JITTER_DETECT_WINDOW_PACKETS），与 drift rebase 共用计数。

// 窗口内 late >= 此值 → target +1 包（快升：1% late 即响应）。
inline constexpr std::uint32_t JITTER_DETECT_RAISE_LATE_COUNT = 5;

// 连续干净窗口（late=0）达到此数 → target -1 包（慢降：~12s/步 @48kHz）。
// 中间带（0 < late < raise 阈值）为迟滞区：不升不降且打断连续干净计数。
// 实测（44.1kHz 回环极端测试）：4 个窗口（~6.5s）会在 late 突发间隔内
// 过早回落，与下一次突发形成 3↔4 翻转（7 分钟 9 次），且降档排水 1 包
// 恰好放大脆弱期余量；8 个窗口（~13s）要求突发真正平息后才降。
inline constexpr std::uint32_t JITTER_DETECT_LOWER_CLEAN_WINDOWS = 8;

// ---- 运行时可配置参数 ----
// 前端（CLI / UI）填充此结构体后传入 core 组件构造函数。
// core 不依赖全局状态，所有可调参数通过此结构体注入。
struct RuntimeConfig {
    // JitterBuffer 总容量（毫秒）。唯一 JB 用户面参数，floor/ceiling/capacity
    // 包数由此自动推导（规则见 DEFAULT_JITTER_BUFFER_MS 处注释）。
    // 0 = DEFAULT_JITTER_BUFFER_MS。
    std::uint32_t jitter_buffer_ms = DEFAULT_JITTER_BUFFER_MS;

    // 播放 RingBuffer 大小（字节）
    std::size_t playback_ringbuffer_size = DEFAULT_PLAYBACK_RINGBUFFER_BYTES;

    // 采集 RingBuffer 大小（字节）
    std::size_t capture_ringbuffer_size = DEFAULT_CAPTURE_RINGBUFFER_BYTES;
};

} // namespace aqua::config

#endif // AQUA_CONFIG_H
