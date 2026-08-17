# 模块与接口规范

## 1. SessionManager

`src/core/session/session_manager.{h,cpp}`

只负责 Session 生命周期和 UDP NAT endpoint，不负责采集/播放/PCM 转换/UDP socket/gRPC/Codec。

```cpp
class SessionManager {
public:
    using session_id_t = std::uint32_t;

    enum class SessionState : uint8_t {
        Created = 0, Connecting = 1, Connected = 2, Expired = 3, Closed = 4,
    };

    struct SessionInfo {
        session_id_t session_id;
        asio::ip::udp::endpoint endpoint;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_seen;
        SessionState state = SessionState::Created;
    };
};
```

### 状态机

当前实际只使用两态（`Created` / `Connected`），超时与 Disconnect 均直接 `remove_session()`：

```text
gRPC Connect -> Created --establish_udp()--> Connected
Created/Connected + timeout/disconnect -> (删除)
```

- `establish_udp()` 是进入 `Connected` 的唯一入口，同时记录 NAT 真实 endpoint 并刷新 last_seen；对已 Connected 幂等（NAT
  remap 更新 endpoint）。
- `for_each_connected(callback)` 快照式遍历（锁内收集 endpoint 副本，锁外回调），回调中可安全调用 SessionManager 方法。
- `collect_expired_sessions()` 只读不删，调用方拿到列表后自行 `remove_session`。

### 线程安全

`std::shared_mutex`：读操作（get / get_endpoint / is_connected / count / collect_expired /
for_each）持共享锁；写操作（create / remove / establish / touch / clear）持排他锁。禁止在持有 SessionInfo 引用期间回调
SessionManager。

### Session ID 生成

`16 bit 随机 instance_id（构造时 |1 保证非零）+ 16 bit 自增 counter = 32 bit session_id`。跨进程靠 instance_id 区分，仅保证
Server 生命周期内尽量不冲突。

## 2. RingBuffer

`src/core/audio/ringbuffer/spsc_ringbuffer.{h,cpp}`

单生产者单消费者无锁环形缓冲（音频回调 ↔ 网络线程）。

```cpp
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity_bytes);
    std::size_t write(std::span<const std::byte> data) noexcept; // 返回实际写入
    std::size_t read(std::span<std::byte> out) noexcept;         // 返回实际读出
    std::size_t available_read() const noexcept;
    std::size_t available_write() const noexcept;
    std::size_t capacity() const noexcept;  // 已向上取整为 1KiB 倍数
    void clear() noexcept;                  // 仅消费者调用
};
```

- 容量向上取整为 1KiB（1024）的倍数（`RINGBUFFER_ALIGNMENT_BYTES`），索引用取模（非 2 的幂）。
- 写满返回实际写入量（不阻塞、不覆盖未读数据），调用方负责丢弃/统计。
- `write_pos_` / `read_pos_` 用 `alignas(64)` cache line 对齐，避免 false sharing。
- 热路径零 heap allocation。

## 3. JitterBuffer

`src/core/jitter_buffer/jitter_buffer.{h,cpp}`

在固定播放延迟下处理 UDP 乱序、重复、丢包和 late packet。 **push 时不判丢包，playout deadline 才判定 lost**。

### 构造

```cpp
JitterBuffer(const AudioFormat& format,
             std::uint32_t frames_per_packet,
             std::size_t floor_packets,      // 起播缓冲包数（自适应下限）
             std::size_t capacity_packets,   // 2 的幂，>= floor*2
             std::uint32_t detect_window_packets = config::JITTER_DETECT_WINDOW_PACKETS,
             std::uint32_t drift_rebase_late_count = config::JITTER_DRIFT_REBASE_LATE_COUNT,
             std::optional<AdaptiveTargetConfig> adaptive = std::nullopt);
```

### 关键行为

- **内存**：预分配连续 PCM storage（`slots_` metadata + `storage_` PCM 分区），热路径零分配；slot 空闲用 `bool valid`（不用
  `sequence == 0`）。
- **时间线**：基于 `first_packet_time_ + target_latency * packet_duration_`（计数式启动之外的「时间线启动」），不依赖
  timer（timer 是外部调度器）。
- **sequence 回绕**：`int32_t` 有符号差值比较。
- **rebase 保持节奏**：小缺口沿原 cadence 推进 deadline（PLC 填补），只有大于 target 的断裂才重新缓冲。
- **reset ()** 只清 slot + timeline，不清 storage_ 与统计计数器。
- **静音/丢包隐藏**：PLC（上一包衰减重复），连续丢包增益减半收敛为静音；S24/U8 回退纯静音。
- **自适应 target**（可选，客户端恒启用）：late 压力抬升、干净窗口回落，区间 [floor, ceiling]，通过 `next_deadline_ ± 1 拍`
  蓄水/排水。

线程契约：`push` 与 `pop_next` 必须在同一线程（io_context 单线程）；诊断 getter 带锁可从其他线程读。

### 与 RingBuffer 的职责边界

JitterBuffer 管 packet 时间顺序 / reorder / 去重 / late / loss / playout deadline / concealment；RingBuffer 只管 PCM
字节流跨线程传递，不感知 packet/sequence。

## 4. Clock Synchronization

当前阶段（M4/M5） **不做 clock synchronization，只做 clock drift 诊断**。音频时间轴以 sample 为基础（
`sample_position + steady_clock`），不依赖 wall clock / NTP。

- 命名用 **estimated playback-rate drift**（而非 clock drift）：由 RingBuffer occupancy 趋势推断 producer/consumer
  速率差，不是直接测量晶振。
- 不使用 resampler；correction 策略待 M5 实测数据决定。

## 5. Client Audio Format Conversion

Server 永远发送固定 AudioFormat；Client 接收后自行决定是否转换（重采样/转码）。Server 不参与转换。

## 6. 模块接口规范

### 6.1 logger

`src/core/logger/logger.h`：`set_log_level` + `log_trace/debug/info/warn/error`（string_view）+ `log_*_fmt`（spdlog 格式化）。
热路径只允许 `log_trace`/`log_debug`，禁止 `_fmt` 排版复杂对象。

### 6.2 audio_format

见 [protocol.md](protocol.md)。

### 6.3 net/transport

`src/core/net/transport/udp_transport.{h,cpp}`：基于 `asio::io_context`，异步收发。

- `bind(bind_ip, port)` / `start_receive(handler)` / `send(target, data)` / `stop()` / `is_open()` /
  `socket_local_endpoint()`。
- 接收缓冲预分配 65536 字节；`send` 内部 `asio::post` 到 io_context 线程，避免跨线程访问 socket。
- 接收循环遇非 `operation_aborted` 错误（ICMP port unreachable）不终止，继续投递。

### 6.4 net/packet

见 [protocol.md](protocol.md)。

### 6.5 grpc

- `AudioServiceImpl`：持有 SessionManager 引用（不拥有），Connect 内部 `create_session()`。
- `GrpcServer`：构造时同步 `BuildAndStart()`，失败 `is_running()` 返回 false；`run()` 阻塞，`shutdown()` 非阻塞。
- `GrpcClient`：`connect_to_server` 等 channel 就绪最多 5s；`disconnect` 设 500ms 短超时（best-effort）。

### 6.6 audio/backend

`src/core/audio/backend/audio_backend_factory.h`：

```cpp
class CaptureBackend {
    virtual bool start(CaptureCallback cb, AudioFormat& out_format) = 0; // 阻塞至初始化完成
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

class PlaybackBackend {
    virtual bool start(AudioFormat format, FillCallback cb) = 0; // 阻塞至初始化完成
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};
```

- 平台实现（wasapi / aaudio）不得泄漏到接口（头文件不含平台头）。
- 回调在音频实时线程触发，遵守无锁/无分配/无阻塞。
- `is_running()` 基于原子标志，线程因任何原因退出后返回 false。

### 6.7 运行时（编排层）

`src/core/server/server_runtime.h` / `src/core/client/client_runtime.h`。

- `ServerRuntime`：`start(cfg, cb)` 同步启动全部子系统；`run(stop_when)` 阻塞健康监控；`shutdown()` 非阻塞。
- `ClientRuntime`：`start(cfg, cb)` 异步启动会话线程；`run(stop_when)`；`shutdown()` 非阻塞。
- 组件是「工具箱」，运行时是「装配线」；CLI / C API / UI 只面向运行时。
- 用 pImpl 隔离实现，头文件不含 Asio / gRPC / 平台音频类型。

生命周期契约：`start()` 失败返回 false 且 `last_error()` 有原因；`run()` 返回前完成资源清理与线程 join，返回后 `on_stopped`
已触发；`shutdown()` 仅置位原子标志（signal-safe）；回调在内部线程触发不得阻塞。

## 7. C API 边界（UI ↔ Core）

`include/aqua.h`（权威定义）。

- 只暴露不透明句柄 `aqua_client_t*` / `aqua_server_t*`，不暴露 C++ 类布局。
- 所有函数返回 `aqua_status_t`（0 = OK，<0 = 错误），`start()` 失败详因经 `last_error()`。
- 跨边界不抛异常（内部 try/catch 全捕获）、不传 `std::string`（统一 `const char*` UTF-8）。
- 字符串入参在 `start()` 时拷贝；出参指针在下次调用同函数或句柄销毁前有效。
- 回调 struct 与 `user_data` 在 `start()` 时按值拷贝；回调在 core 内部线程触发，不得阻塞。
- `aqua_encoding_t` 与内部 `AudioEncoding` 有 6 条 `static_assert` 编译期同步。

## 8. 配置策略

- Server CLI：`--bind-ip` / `--rpc-port` / `--udp-port` / `--capture-buffer` / `--log-level`。
- Client CLI：`--server-ip` / `--server-rpc-port` / `--jitter-buffer` / `--jitter-detect-window` / `--playback-buffer` /
  `--auto-reconnect` / `--log-level`。
- 超时/保活常量集中在 `src/core/public/config.h`（`SESSION_TIMEOUT` / `HELLO_KEEPALIVE_INTERVAL` /
  `CLIENT_AUDIO_RECV_TIMEOUT` 等）。
- `RuntimeConfig` 结构体集中管理可调参数，core 不依赖全局状态；CLI 值为 0 时用 config.h 默认。
- **不引入 YAML/JSON/TOML 配置文件**，第一阶段走 CLI + 编译期常量。

## 9. 日志规范

级别：Trace（逐包）/ Debug（状态机迁移、keepalive、周期统计）/ Info（启动停止、session 建立）/ Warn（丢包、解码失败、rebase）/
Error（gRPC 失败、bind/设备失败）。

- 高频路径 5s 周期统计输出（WASAPI capture/playback、packetizer、client 接收、JitterBuffer rebase 仅 Warn）。
- 每条日志尽量含 `session_id`（`0x{:08X}`）、`endpoint`、`sequence`。
- 默认级别：Debug preset = Debug，Release = Info。
