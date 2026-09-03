# 配置参考

本文列出所有默认值与限制。常量的权威定义位置：

- `aqua_core/include/aqua/runtime/runtime_config.h`（runtime 与端口默认）
- `aqua_core/include/aqua/runtime/runtime_state.h`（control poll）
- `aqua_core/include/aqua/net/udp/udp_config.h`（UDP 与 session）
- `aqua_core/include/aqua/net/grpc/grpc_config.h`（gRPC）
- `aqua_core/include/aqua/audio/audio_format.h`（格式上限）
- `aqua_core/include/aqua/audio/buffer/jitter_buffer.h`（缓冲策略与重锚定）

## 1. 网络与会话

| 常量                          |      值 | 定义位置                     |
|-------------------------------|--------:|------------------------------|
| `DEFAULT_RPC_PORT`            |   50051 | `runtime_config.h`           |
| `DEFAULT_UDP_PORT`            |   50000 | `runtime_config.h`           |
| `DEFAULT_BIND_IP`             | `0.0.0.0` | `runtime_config.h`         |
| `DEFAULT_CLIENT_NAME`         | `aqua-client` | `runtime_config.h`     |
| `UDP_RECV_BUFFER_BYTES`       |   65536 | `udp_config.h`               |
| `UDP_SEND_BUFFER_BYTES`       |   65536 | `udp_config.h`               |
| `UDP_AUDIO_PAYLOAD_BYTES`     |    1443 | `udp_config.h`（1500−40−8−9）|
| `UDP_MAX_QUEUED_DATAGRAMS`    |      64 | `udp_config.h`               |
| `SESSION_TIMEOUT`             |  5000 ms| `udp_config.h`               |
| `SESSION_REAP_INTERVAL`       |  1000 ms| `udp_config.h`               |
| `HELLO_INTERVAL`              |  1000 ms| `udp_config.h`               |
| `HELLO_ACK_MISS_THRESHOLD`    |       3 | `udp_config.h`               |
| `GRPC_CONNECT_DEADLINE`       |  3000 ms| `grpc_config.h`              |
| `GRPC_DISCONNECT_DEADLINE`    |  1000 ms| `grpc_config.h`              |
| `GRPC_MAX_CLIENT_NAME_BYTES`  |     128 | `grpc_config.h`              |

## 2. 音频几何与缓冲

| 常量                                 |   值 | 说明                                    |
|--------------------------------------|-----:|-----------------------------------------|
| `DEFAULT_CLIENT_JITTER_BUFFER_SLOTS` |   30 | client 抖动缓冲槽数（`--jitter-slots`） |
| `MIN_JITTER_BUFFER_SLOTS`            |    4 | 下限（= `JITTER_BUFFER_MIN_CAPACITY_SLOTS`） |
| `MAX_JITTER_BUFFER_SLOTS`            | 4096 | 上限                                    |
| `DEFAULT_SERVER_NETWORK_QUEUE_SLOTS` |   16 | server 交接队列槽数（`--network-queue-slots`） |
| `MAX_NETWORK_QUEUE_SLOTS`            | 4096 | 上限                                    |
| `MIN_FRAMES_PER_SLOT`                |   16 | 显式 F 的下限                           |
| `AUDIO_FORMAT_MAX_CHANNELS`          |   64 | 声道上限                                |
| `AUDIO_FORMAT_MAX_SAMPLE_RATE`       | 768000 | 采样率上限                            |
| `AudioPlaybackConfig::frames_per_buffer` | 480 | 回放请求粒度（`audio_playback_config.h`） |
| `AudioCaptureConfig::frames_per_buffer` | 0 | 采集由后端决定（`audio_capture_config.h`） |

F 的推导：`frame_count_for_budget(F_budget) = floor(UDP_AUDIO_PAYLOAD_BYTES / frame_bytes)`。显式 F 需满足
`F >= 16` 且 `F × frame_bytes <= 1443`，否则启动被拒。

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
JITTER_BUFFER_MAX_REANCHOR_JUMP_FRAMES = 100000   超过则判为荒谬并拒绝该帧
JITTER_BUFFER_REANCHOR_MIN_GAP         = 4        缺口达到该值才受理 reanchor 请求
JITTER_BUFFER_REANCHOR_HOLD_STUCK_PULLS= 5        Hold 无进展时强制应用
```

## 6. Android App 默认值

App 复用第 1–5 节的 Core 默认值，下表是 App 层自有默认。参数经 C API 透传给 `ClientRuntime`，语义与 CLI 一致——
`0` / 空 / `-1` 表示"沿用 Core 默认"。

| 参数             | App 默认            | C API 字段                   | CLI 等价           | 说明                                |
|------------------|---------------------|------------------------------|--------------------|-------------------------------------|
| 服务器 IP        | `192.168.1.100`     | `server_ip`                  | `--server-ip`      | 首页可编辑；留空回退 `127.0.0.1`    |
| RPC 端口         | `50051`             | `rpc_port`                   | `--server-rpc`     | 1..65535；非法回退 50051            |
| 抖动缓冲槽数     | 0（Core 默认 30）   | `jitter_buffer_slots`        | `--jitter-slots`   | 0=默认；显式 4..4096（UI 上限 400） |
| HELLO 间隔       | 0（Core 默认 1000ms）| `hello_interval_ms`         | —                  | 0=默认；UI 0..2000 ms               |
| 客户端名称       | `aqua_android`      | `client_name`                | `--name`           | Core 默认 `aqua-client`，App 覆盖   |
| UDP 端口覆盖     | 空（用 server 通告）| `force_udp_port`             | `--force-udp-port` | NAT / 端口映射场景                  |
| 日志级别         | -1（Info）          | `log_level`                  | `--log-level`      | 0..5 = Trace..Fatal                 |
| playback 帧/回调 | 0（backend 自适应） | `playback_frames_per_buffer` | —                  | AAudio 决议：不显式指定             |

App 层自有设置（不进入 Core）：

| 设置             | 默认 | 说明                                                            |
|------------------|------|-----------------------------------------------------------------|
| 自动重连         | 关   | 播放异常停止 3s 后后台重连（UI 层实现；core 契约为"终态即停"）   |
| 播放时屏幕常亮   | 关   | 播放期间保持屏幕常亮                                            |
| 允许同时播放     | 关   | 关 = 播放时持有音频焦点；开 = 不申请焦点、与其它 App 共存        |
| 自动切换播放设备 | 开   | 决定连接起步路由：开 = FollowSystem，关 = PreferCurrent（钉住首流设备）|
| 低延迟模式       | 关   | 对应 AAudio `PERFORMANCE_MODE_LOW_LATENCY`                      |

## 7. 不能通过 CLI 修改的协议固定项

- Audio wire header 9 bytes；HELLO / HELLO_ACK 5 bytes；
- Audio sequence 为 u64，session id 为 u32，wire 为小端；
- 一个 datagram 承载一个完整 AudioFrame；
- Server 一次运行期间 `AudioFormat` 与 F 固定（设备可切换，格式不可变）。
