# 配置参考

## Core 默认值

| 参数 | 默认 |
|---|---:|
| gRPC port | 50051 |
| UDP port | 9999 |
| bind IP | `0.0.0.0` |
| client name | `aqua-client` |
| client JitterBuffer | 30 slots |
| server network handoff queue | 4 slots |
| min JitterBuffer | 4 slots |
| max JitterBuffer | 4096 slots |
| max network queue | 4096 slots |
| MIN_FRAMES_PER_SLOT | 16 frames |
| UDP recv buffer | 65536 bytes |
| UDP send buffer | 65536 bytes |
| UDP audio payload budget | 1443 bytes |
| UDP queued datagrams | 64 |
| HELLO interval | 1000 ms |
| session timeout | 5000 ms |
| session reap interval | 1000 ms |
| HELLO ACK miss threshold | 3 |
| gRPC connect deadline | 3000 ms |
| gRPC disconnect deadline | 1000 ms |
| max client name | 128 bytes |
| diagnostics snapshot | 1000 ms |
| runtime control poll | 500 ms |

## JitterBuffer 默认策略

```text
capacity      = client config (default 30 slots)
target        = 0.60N
normal_low   = 0.45N
normal_high  = 0.75N
warning_low  = 0.30N
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
