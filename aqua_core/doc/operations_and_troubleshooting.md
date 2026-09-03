# 运维与故障排查

## 1. Client 连不上

先看：

```text
gRPC connect
ConnectResponse valid?
UDP remote configured?
UDP receive started?
HELLO ack count/misses
```

若 gRPC 成功、HELLO 连续 miss=3：问题通常在 UDP 路径、防火墙、advertised address 或目标 endpoint，而不是 JitterBuffer。

## 2. Client 有连接但没声音

按顺序看：

1. `playback_running`
2. `audio_error`
3. Server `frames_broadcast`
4. Client UDP `rx_packets/rx_bytes`
5. `udp_audio_payload_mismatches`
6. `jitter_push_accepted`
7. `jitter_pull_silence_frames`
8. Jitter `water/used/capacity`

### 常见解释

- `rx=0`：先查 UDP。
- `rx>0` 但 `push_ok=0`：查 payload geometry/sequence/slot collision。
- `push_ok>0` 但长期 `silence_frames≈pull_frames`：查 pre-roll、水位、缺帧或 playback callback。
- `water` 长期 > 90%：发送快于消费或时间线失配，观察 Drop/reanchor。
- `water` 长期 < 30%：网络供给不足或播放消费快于接收，观察 Fill/缺帧静音。

## 3. 高负载下丢包

Server 重点观察：

- capture starved
- queue dropped
- dispatcher encode/dispatch failures
- UDP tx dropped / enqueue failures

Capture RT 到 network worker 中间只有有界 queue；它满时丢最新 frame，这是有意的 backpressure policy。不要因此把 Server
queue 当作网络 jitter buffer。

## 4. JitterBuffer 问题

优先打开 Debug，再短时间开启 RT debug log。重点关注：

```text
pre-roll anchor
FILL enter/complete
DROP enter/complete
REANCHOR request/apply/cancel
slot busy / late / sanity reject
```

长期记录 RT debug 日志会改变 timing，不应作为性能基线。

## 5. 地址问题

IPv6 一律使用：

```text
[addr]:port
```

内部 `parse_ip_address()` 只接受 IP literal，不解析主机名。CLI client 的 `--server-ip` 明确拒绝 unspecified address 和非 IP
主机名；CLI server 的 `--server-ip` 与 `--advertise-ip` 允许 wildcard（server 要监听所有网卡），但通告 wildcard 时 client 会
回退到 gRPC 连接所用的地址。

## 6. 设备切换

设备失效**不再**终止进程。排查时先看切换维度，再看音频维度：

```text
Server：capture_switch.state / route / last_outcome / last_switch_error
Client：playback_state / route_mode / switch_outcome / switch_error
```

| 现象                                     | 含义与处理                                                          |
|------------------------------------------|---------------------------------------------------------------------|
| `switch=switching` 长时间不变            | 事务卡在设备打开；看日志中哪个候选在失败                             |
| `last_switch=rolled_back`                | 目标设备不可用，已回到先前的实际设备（临时降级，用户意图未变）       |
| `last_switch=fell_back_to_system`        | 目标与回滚都失败，落到了系统默认                                     |
| `switch=fatal`                           | 候选链耗尽或 10s 内超过 3 次自动 restart；会话会被终止，看最后一个错误原因 |
| 频繁切换（每次间隔 < 10s）               | 设备插拔风暴；达到预算上限后会 Fatal，属预期保护                     |
| 切换后 client 短暂无声                   | 预期：server 切换是 packet gap，由 client JitterBuffer 的饥饿路径吸收 |

日志关键字（Debug 级）：

```text
capture device error ..., switch pending          错误已上报，等 control tick
CaptureManager switch begin / completed           事务开始与结果（含候选数）
system default device changed ... following       跟随系统默认设备变化
capture switch fatal (fallback chain exhausted)   链耗尽，进程退出
```

不要用"静音"或"低能量"判断设备故障：loopback 在没有 render client 时静默并产出合成静音是合法稳态。只有
`DeviceDisconnected` 与设备列表变化会触发切换。

## 7. 与 Degraded 的区分

- **切换中 / 已切换**：会话保持 `Running`，`last_switch_error` 记录原因（成功后清零错误通道）。
- **`Degraded`**：不可自愈的终止条件——非设备的后端错误，或切换 Fatal。CLI 下一 tick 停止进程，退出码为 0（脚本无法据此
  区分"正常退出"与"故障退出"，排障要看日志末尾）。
