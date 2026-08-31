# 协议

## 1. 双平面

Aqua 使用两个逻辑通道：

```text
Control plane  = gRPC TCP
Data plane     = UDP
```

gRPC 只做 session 生命周期和音频流参数下发，不负责保活和音频传输。

## 2. Connect

客户端调用：

```text
ConnectRequest {
    client_name
}
```

`client_name` 必须 1..128 bytes。

服务端成功返回：

```text
ConnectResponse {
    session_id
    udp { address, port }
    audio_format { encoding, channels, sample_rate }
    frame_count = F
}
```

### 地址语义

`udp.address` 可以是 `0.0.0.0` / `::`。这不是让 client 向 wildcard 地址发送，而是一个 sentinel：client 回退到它原始连接
gRPC 时使用的具体 `server_ip`，UDP 端口仍使用 response 中的 port。

因此：

```text
Server 监听地址可以与 advertised UDP 地址不同
```

完全独立。

## 3. Session

Connect 创建一个 `SessionManager` entry，初始状态 `Created`。此时还没有可信 UDP endpoint。

只有 UDP HELLO 成功后才变为 `Connected`，并记录实际 sender endpoint。

## 4. UDP wire format

所有整数明确使用 little-endian。

### Audio

```text
byte 0      : type = 3
byte 1..8   : sequence (u64 LE)
byte 9..    : PCM payload
```

单 datagram 只承载一个完整 `AudioFrame`，payload 上限 1443 bytes。

`frame_count` 不进入 datagram，因为它已由 Connect 下发，并在一次 server run 内固定。

### HELLO / HELLO_ACK

```text
byte 0      : type = 1 / 2
byte 1..4   : session_id (u32 LE)
```

长度必须严格等于 5 bytes。

## 5. HELLO 保活

默认：

```text
HELLO_INTERVAL = 1000 ms
SESSION_TIMEOUT = 5000 ms
REAP_INTERVAL = 1000 ms
HELLO_ACK_MISS_THRESHOLD = 3
```

Server 收到 HELLO：

1. decode；
2. 必须是 HELLO；
3. `SessionManager::establish_session(session_id, sender)`；
4. 更新 endpoint 和 last_seen；
5. reply HELLO_ACK。

**只有 HELLO 更新 last_seen。Audio datagram 不更新。**

Client 每次 HELLO 都等待 ack 计数：收到 ack 后 miss counter 清零；连续达到 3 次 miss 后触发 liveness failure callback。该
failure 不自动等价于强制重连，具体由 Runtime/上层处理。

### Client UDP endpoint discovery

客户端在数据面**不要求** HELLO_ACK 的来源 endpoint 与 gRPC 通告的 server endpoint 一致（IPv6 隐私扩展 / 多地址服务器下，ACK 源地址可与 gRPC 地址不同）：

```text
HELLO_ACK:
    校验 session_id == 当前会话（不校验来源地址）
    通过 → learned_endpoint = sender（学习/刷新实际对端）

Audio:
    learned_endpoint 为空（尚未握手）→ 丢弃
    sender != learned_endpoint           → 丢弃
    否则接受
```

语义边界：

- session_id 负责会话身份，learned_endpoint 负责 UDP 来源约束；
- HELLO_ACK 负责发现/刷新 endpoint，每次有效 ACK 都重锁；
- Audio 帧不携带 session_id，只能严格匹配当前 learned_endpoint。


## 6. Audio 接收校验

Client UDP 接收 loop 在启动时拿到 expected payload bytes：

```text
expected = F × frame_bytes
```

Audio datagram 只有在 payload 大小严格等于 expected 时才送入 JitterBuffer；否则统计 `audio_payload_mismatches` 并丢弃。

这保证 JitterBuffer 不需要在 RT/网络边界重复推断格式。

## 7. Disconnect

Disconnect 是 best-effort：

- session 不存在仍视为幂等成功语义；
- RPC 失败不会阻塞 client stop，默认 deadline 1s；
- Server 最终也会在 stop 时 clear 所有 sessions。

## 8. Trust model

当前 HELLO 只携带 session_id，没有认证 token。知道一个合法 session_id 的主机可以伪造 HELLO 覆盖
endpoint。因此当前实现适合可信内网/实验环境。

公网部署不能直接视为安全协议；未来应在 ConnectResponse 增加随机 token，并将 token 纳入 HELLO 校验。
