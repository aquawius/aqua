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

Capture RT 到 network worker 中间只有有界 queue；它满时丢最新 frame，这是有意的 backpressure policy。不要因此把 Server queue 当作网络 jitter buffer。

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

内部 `parse_ip_address()` 只接受 IP literal，不解析主机名。CLI `--server-ip` 也明确拒绝 unspecified address 和非 IP 主机名。
