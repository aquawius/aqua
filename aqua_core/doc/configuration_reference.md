# 配置参考

## Core 默认值

| 参数                                       |          默认 |
|--------------------------------------------|--------------:|
| gRPC port                                  |         50051 |
| UDP port                                   |         50000 |
| Server 监听 IP（gRPC 与 UDP 共用）         |     `0.0.0.0` |
| UDP 通告 IP（未指定时跟随 Server 监听 IP） |     `0.0.0.0` |
| UDP 通告端口（未指定时跟随 UDP 端口）      |       `50000` |
| client name                                | `aqua-client` |
| client JitterBuffer                        |      90 slots |
| server network handoff queue               |      12 slots |
| min JitterBuffer                           |       4 slots |
| max JitterBuffer                           |    4096 slots |
| max network queue                          |    4096 slots |
| MIN_FRAMES_PER_SLOT                        |     16 frames |
| UDP recv buffer                            |   65536 bytes |
| UDP send buffer                            |   65536 bytes |
| UDP audio payload budget                   |    1443 bytes |
| UDP queued datagrams                       |            64 |
| HELLO interval                             |       1000 ms |
| session timeout                            |       5000 ms |
| session reap interval                      |       1000 ms |
| HELLO ACK miss threshold                   |             3 |
| gRPC connect deadline                      |       3000 ms |
| gRPC disconnect deadline                   |       1000 ms |
| max client name                            |     128 bytes |
| diagnostics snapshot                       |       1000 ms |
| runtime control poll                       |        500 ms |

## Android App 参数

Android App 复用上表 Core 默认值；下表是 App 层自有默认值。参数经 C API（`aqua_capi`）透传给
`ClientRuntime`，语义与 CLI 一致——`0` / 空 / `-1` 均表示"沿用 Core 默认"。

| 参数               | App 默认              | C API 字段                   | CLI 等价           | 说明                                       |
|--------------------|-----------------------|------------------------------|--------------------|--------------------------------------------|
| 服务器 IP          | `192.168.1.100`       | `server_ip`                  | `--server-ip`      | 首页可编辑；留空回退 `127.0.0.1`           |
| RPC 端口           | `50051`               | `rpc_port`                   | `--server-rpc`     | 1..65535；非法回退 50051                   |
| 抖动缓冲槽数       | 0（Core 默认 90）     | `jitter_buffer_slots`        | `--jitter-slots`   | 0=默认；显式 4..4096（UI 上限 400）        |
| HELLO 间隔         | 0（Core 默认 1000 ms）| `hello_interval_ms`          | —                  | 0=默认；UI 0..2000 ms                      |
| 客户端名称         | `aqua_android`        | `client_name`                | `--name`           | Core 默认 `aqua-client`，App 覆盖          |
| UDP 端口覆盖       | 空（server 通告）     | `force_udp_port`             | `--force-udp-port` | NAT/端口映射；非法同 0                     |
| 日志级别           | -1（默认 Info）       | `log_level`                  | `--log-level`      | 0..5 = Trace..Fatal                        |
| playback 帧/回调   | 0（backend 自适应）   | `playback_frames_per_buffer` | —                  | AAudio 决议：不显式指定                    |

App 层设置（不进入 Core，仅 App 语义）：

| 设置             | 默认 | 说明                                                          |
|------------------|------|---------------------------------------------------------------|
| 自动重连         | 关   | 播放异常停止 3s 后后台重连（UI 层实现，core 契约为"终态即停"）|
| 播放时屏幕常亮   | 关   | 播放期间保持屏幕常亮                                          |
| 允许同时播放     | 关   | 关=播放时持有音频焦点（他方 App 自动暂停）；开=不申请焦点、共存 |

Android 端日志默认 Info（logcat）；`log_level` 仅在 ≥0 时调整进程级别。Android 只实现 playback
backend（AAudio），capture / OUTPUT_LOOPBACK 未实现（见 `android_roadmap.md`、`aaudio_backend_design.md`）。

## JitterBuffer 默认策略

```text
capacity      = client config (default 90 slots)
target        = 0.60N
normal_low   = 0.35N
normal_high  = 0.80N
warning_low  = 0.20N
warning_high = 0.90N
```

默认 warning step：

```text
min_step = 1
max_step = max(2, round(0.10N))
growth = 2.0
growth interval = 4 warning evaluations
```

## 不能通过 CLI 修改的协议固定项

- Audio wire header = 9 bytes
- HELLO/ACK = 5 bytes
- Audio sequence = u64
- Session ID = u32
- wire little-endian
- 一包一个完整 AudioFrame
- Server 一次运行固定 AudioFormat/F
