# 配置参考

本文列出所有默认值与限制。常量的权威定义位置：

- `aqua_core/include/aqua/runtime/runtime_config.h`（runtime 与端口默认）
- `aqua_core/include/aqua/runtime/runtime_state.h`（control poll）
- `aqua_core/include/aqua/net/udp/udp_config.h`（UDP 与 session）
- `aqua_core/include/aqua/net/grpc/grpc_config.h`（gRPC）
- `aqua_core/include/aqua/audio/audio_format.h`（格式上限）
- `aqua_core/include/aqua/audio/buffer/buffer_config.h`（Buffer 组件全部默认值与策略常量）

## 1. 网络与会话

| 常量                          |      值 | 定义位置                     |
|-------------------------------|--------:|------------------------------|
| `DEFAULT_RPC_PORT`            |   50051 | `runtime_config.h`           |
| `DEFAULT_UDP_PORT`            |   50000 | `runtime_config.h`           |
| `DEFAULT_BIND_IP`             | `0.0.0.0` | `runtime_config.h`         |
| `DEFAULT_CLIENT_NAME`         | `aqua-client` | `runtime_config.h`     |
| `UDP_RECV_BUFFER_BYTES`       |   65536 | `udp_config.h`               |
| `UDP_SEND_BUFFER_BYTES`       |   65536 | `udp_config.h`               |
| `UDP_AUDIO_PAYLOAD_BYTES`     |    1440 | `udp_config.h`（1500−40−8−12）|
| `UDP_MAX_QUEUED_DATAGRAMS`    |      64 | `udp_config.h`               |
| `SESSION_TIMEOUT`             |  5000 ms| `udp_config.h`（只看 proto Keepalive 刷新的 last_seen）|
| `SESSION_REAP_INTERVAL`       |  1000 ms| `udp_config.h`               |
| `HEARTBEAT_HANDSHAKE_INTERVAL`              |  1000 ms| `udp_config.h`（握手期 heartbeat 节奏）|
| `HEARTBEAT_INTERVAL`          |  1000 ms| `udp_config.h`（association 建立后）|
| `HEARTBEAT_HANDSHAKE_ACK_MISS_THRESHOLD`    |       3 | `udp_config.h`（握手期：连续 3 周期无 ACK 即建连失败）|
| `HEARTBEAT_ACK_MISS_THRESHOLD`|       5 | `udp_config.h`（稳态：连续 5 周期无 ACK 即路径死亡）|
| `GRPC_CONNECT_DEADLINE`       |  3000 ms| `grpc_config.h`              |
| `GRPC_DISCONNECT_DEADLINE`    |  1000 ms| `grpc_config.h`              |
| `GRPC_MAX_CLIENT_NAME_BYTES`  |     128 | `grpc_config.h`              |
| `GRPC_KEEPALIVE_INTERVAL`     |  1000 ms| `grpc_config.h`（proto 探活节奏）|
| `GRPC_KEEPALIVE_DEADLINE`     |   800 ms| `grpc_config.h`（单次超时；必须 < interval）|
| `GRPC_KEEPALIVE_MISS_THRESHOLD`|      5 | `grpc_config.h`（传输连续失败阈值；SessionGone 立即上报）|

## 2. 音频几何与缓冲

| 常量                                 |   值 | 说明                                    |
|--------------------------------------|-----:|-----------------------------------------|
| `DEFAULT_CLIENT_JB_CAPACITY_SLOTS` |   30 | client 抖动缓冲槽数（`--jb-capacity`） |
| `MIN_JB_CAPACITY_SLOTS`            |    4 | 下限（= `JITTER_BUFFER_MIN_CAPACITY_SLOTS`） |
| `MAX_JB_CAPACITY_SLOTS`            |  512 | 上限（reanchor O(N) 扫描的 RT 护栏）  |
| `DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS` |   16 | server 交接队列槽数（`--audio-queue-capacity`） |
| `MAX_AUDIO_QUEUE_CAPACITY_SLOTS`            | 4096 | 上限                                    |
| `MIN_FRAMES_PER_SLOT`                |   16 | 显式 F 的下限                           |
| `AUDIO_FORMAT_MAX_CHANNELS`          |   64 | 声道上限                                |
| `AUDIO_FORMAT_MAX_SAMPLE_RATE`       | 768000 | 采样率上限                            |
| `AudioPlaybackConfig::frames_per_buffer` | 480 | 回放请求粒度（`audio_playback_config.h`） |
| `AudioCaptureConfig::frames_per_buffer` | 0 | 采集由后端决定（`audio_capture_config.h`） |

F 的推导：`frame_count_for_budget(F_budget) = floor(UDP_AUDIO_PAYLOAD_BYTES / frame_bytes)`。显式 F 需满足
`F >= 16` 且 `F × frame_bytes <= 1440`，否则启动被拒。

## 3. 运行期节奏

| 常量                             |    值 | 说明                                          |
|----------------------------------|------:|-----------------------------------------------|
| `RUNTIME_CONTROL_POLL_INTERVAL`  | 500 ms| control tick（server 切换 / client 恢复与跟随） |
| `DIAGNOSTICS_SNAPSHOT_INTERVAL`  | 1000 ms| 诊断快照与输出                               |
| 设备事件合并窗口                 | 1000 ms| client 侧 `notify_devices_changed` 去抖（`client_runtime.h`） |

## 4. 设备切换

| 项                     | 值                     | 说明                                        |
|------------------------|------------------------|---------------------------------------------|
| 候选链                 | 3 层                   | `[目标设备, 先前的实际设备, 系统默认]`，去重 |
| 自动 restart 预算      | 10s 窗口内 3 次        | 错误驱动与默认跟随**共享**同一预算，超限 Fatal |
| client 显式选择        | 不计数并重置窗口       | 用户手动选择设备（`set_playback_device`）    |
| server 手动切换        | 不提供                 | server 无运行时切换入口，sticky = CLI 配置   |

## 5. JitterBuffer 策略

> 以下常量的权威定义与取值理由见 `buffer_config.h`（`aqua::config::JB_*`），
> 本节只做速查。

```text
capacity      = client 配置（默认 30 slots）
startup_level = 0.50N    启动 pre-roll 锚定水位（独立于稳态阈值序）
target        = 0.60N
normal_low    = 0.35N
normal_high   = 0.80N
warning_low   = 0.20N
warning_high  = 0.90N
```

warning 区步长：

```text
min_step        = 1
max_step        = max(2, round(0.10N))     # max_step=0 时自动取值
growth          = 2.0
growth interval = 4 次连续 warning evaluation
```

重锚定：

```text
config::JB_MAX_REANCHOR_JUMP_FRAMES     = 100000   超过则判为荒谬并拒绝该帧
config::JB_REANCHOR_MIN_GAP_SLOTS       = 4        缺口达到该值才受理 reanchor 请求
config::JB_REANCHOR_HOLD_STUCK_PULLS    = 5        Hold 无进展时强制应用
```

## 5.1 Buffer 组件内部常量（无 CLI 入口）

这几项只有一个很窄的合理区间，已从 CLI 移除；要改就改 `buffer_config.h` 重编译。

| 常量 | 默认 | 说明 |
|---|---|---|
| `JB_ADAPTIVE_TARGET_CAPACITY_RATIO` | 2/3 | 自适应 target 的**结构**上限（上 1/3 留给抖动吸收） |
| `JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS` | 3 | `--jb-min-target` 默认值；有效下限 = max(本值, 几何地板 + 1) |
| `JB_ADAPTIVE_DEFAULT_INITIAL_TARGET_SLOTS` | 4 | 起步 target |
| `JB_ADAPTIVE_STARTUP_MIN_SLOTS` | 3 | 起步 pre-roll 水位的绝对下限 |
| `JB_ADAPTIVE_DEFAULT_JITTER_GAIN` | 5.0 | k（`--jb-jitter-gain` 的默认值） |
| `JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC` | 1.0 | 回落限速（只锁跌） |
| `JB_ADAPTIVE_RISE_DWELL_MS` | 3000 | 涨后锁跌窗口 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS` | 1.0 | 每次欠载抬升下限 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS` | 6 | 反馈抬升累计上限 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC` | 0.5 | 惩罚回落速率 |
| `JB_ADAPTIVE_DEADBAND_SLOTS` | 0 | 死区（固定 0，不要改） |
| `JB_CONCEALMENT_DEFAULT_MAX_SLOTS` | 3 | 连续掩盖上限（包） |
| `JB_ESTIMATOR_REORDER_WINDOW_PACKETS` | 64 | 乱序/重复观测窗 |
| `JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS` | 5.0 | stall 判定（包周期倍数） |
| `JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC` | 10.0 | stall 峰值（近期最坏到达间隙的衰减最大值）的回落速度 |
| `JB_ADAPTIVE_STALL_PEAK_EXTRA_PACKETS` | 1.0 | margin 峰值项 = stall峰值/包周期 + 本值，与 k×J 取 max |
| `JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS` | 8.0 | margin 峰值项上限（stall 是恢复风险信号，不是 steady-state 延迟要求） |

## 6. Android App 默认值

App 复用第 1–5 节的 Core 默认值，下表是 App 层自有默认。参数经 C API 透传给 `ClientRuntime`，语义与 CLI 一致——
`0` / 空 / `-1` 表示"沿用 Core 默认"。

| 参数             | App 默认            | C API 字段                   | CLI 等价           | 说明                                |
|------------------|---------------------|------------------------------|--------------------|-------------------------------------|
| 服务器 IP        | `192.168.1.100`     | `server_ip`                  | `--server-ip`      | 首页可编辑；留空回退 `127.0.0.1`    |
| RPC 端口         | `50051`             | `rpc_port`                   | `--rpc-port`     | 1..65535；非法回退 50051            |
| 抖动缓冲槽数     | 0（Core 默认 30）   | `jb_capacity_slots`        | `--jb-capacity`   | 0=默认；显式 4..512（UI 上限 400；低于 4 水位带无法严格排序；512 是 reanchor O(N) 扫描的 RT 护栏） |
| 自适应 jitter    | 开                  | `jb_fixed_target`（0=开）| `--jb-fixed-target` | 切回既有固定 target/水位 |
| PCM concealment  | 开                  | `jb_disable_concealment`（0=开）| `--jb-no-conceal` | 缺帧 repeat-last + 短淡出；关=硬静音 |
| 自适应 k         | 5.0                 | `jb_jitter_gain`（0/负/非有限=默认）| `--jb-jitter-gain` | margin = max(k×J, min(stall峰值+1包, 8槽))，target = clamp(margin, 下限, 2/3上限)；延迟↔稳定主力旋钮 |
| target 下限      | 3                   | `jb_min_target_slots`（0=默认）| `--jb-min-target`  | 有效下限 = max(本值, 几何地板+1)；只能抬高，压不到地板以下 |
| Heartbeat 间隔       | 0（Core 默认 1000ms）| `heartbeat_handshake_interval_ms`         | —                  | 0=默认；UI 0..2000 ms               |
| 客户端名称       | `aqua_android`      | `client_name`                | `--client-name`    | Core 默认 `aqua-client`，App 覆盖   |
| UDP 端口覆盖     | 空（用 server 通告）| `udp_force_port`             | `--udp-force-port` | NAT / 端口映射场景                  |
| 日志级别         | -1（Info）          | `log_level`                  | `--log-level`      | 0..5 = Trace..Fatal                 |
| playback 帧/回调 | 0（backend 自适应） | `playback_frames_per_buffer` | —                  | AAudio 决议：不显式指定             |

App 层自有设置（不进入 Core）：

| 设置             | 默认 | 说明                                                            |
|------------------|------|-----------------------------------------------------------------|
| 自动重连         | 关   | 播放异常停止 3s 后后台重连（UI 层实现；core 契约为"终态即停"）   |
| 播放时屏幕常亮   | 关   | 播放期间保持屏幕常亮                                            |
| 允许同时播放     | 关   | 关 = 播放时持有音频焦点；开 = 不申请焦点、与其它 App 共存        |
| 自动切换播放设备 | 开   | 决定连接起步路由：开 = FollowSystem，关 = PreferCurrent（钉住首流设备）|
| 低延迟模式       | 开   | 对应 AAudio `PERFORMANCE_MODE_LOW_LATENCY`（与 Android 代码默认一致）|

## 7. 不能通过 CLI 修改的协议固定项

- Audio wire header 12 bytes（RTP，大端）；Heartbeat / HeartbeatAck 5 bytes（小端）；
- Audio wire sequence 为 u16（接收端展开成 u64 extended sequence），timestamp 为 u32
  （媒体时钟），SSRC 为 u32（每 run 随机），session id 为 u32；
- 一个 datagram 承载一个完整 AudioFrame；
- Server 一次运行期间 `AudioFormat` 与 F 固定（设备可切换，格式不可变）。
