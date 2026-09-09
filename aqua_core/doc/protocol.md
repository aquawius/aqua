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

只有 UDP heartbeat 成功后才变为 `Connected`，并记录实际 sender endpoint（以网络包实际来源为准，不相信 client 自称的地址）。

session_id 是 32 位随机数（`std::random_device`，0 保留为无效），创建时检查碰撞。session 只有两个状态，没有 Closed /
Expired 状态——过期与主动断开都直接删除条目。

过期判定：`now - last_seen > SESSION_TIMEOUT`，由 server 的 reaper 每 `REAP_INTERVAL` 扫描一次（见 `modules/session.md`）。

## 4. UDP wire format

Audio 包使用 RTP 12-byte header，大端序（RFC 3550 §5.1，可被 Wireshark 直接 dissect）；
HELLO / HELLO_ACK 沿用既有 5-byte 小端布局（控制面遗留，不动）。

### Audio

```text
byte 0      : 0x80 (V=2, P/X/CC=0)
byte 1      : M=0, PT (PT=96 PCM-LE；M 置位或 PT 非 96 即非法)
byte 2..3   : sequence (u16 BE) 包排序/乱序/丢包检测专用
byte 4..7   : timestamp (u32 BE) 媒体时钟，单位=会话采样率，起始随机
byte 8..11  : SSRC (u32 BE) 发送流随机 ID（每 server run 一组）
byte 12..   : PCM payload
```

单 datagram 只承载一个完整 `AudioFrame`，payload 上限 1440 bytes（1500−40 IPv6−8 UDP−12 RTP）。
没有长度字段（长度由 datagram 边界隐含），也没有 `frame_count`——它已由 Connect 下发并在一次
server run 内固定。

wire sequence 是 16-bit：接收端按 RFC 3550 附录 A 展开成 u64 extended sequence 后再上交，
JB 内部一律 u64，不感知回绕。timestamp 是 media timeline，只解析不判定（estimator 阶段再用）；
sequence 只做 ordering——二者职责分离。

**Audio 帧不携带 session_id**，流身份由 SSRC 承担：client 钉住首包 SSRC（与 learned_endpoint
同模型），不等即丢（计入 `malformed_datagrams`，专用计数器随 estimator 阶段补）；
SSRC == 0 永不接受。来源约束仍是 `learned_endpoint`（见 §5），两者缺一即丢。

编码时 payload 为空或超过 1440 字节会返回空 buffer（不产生 datagram）；解码时要求首字节
`0x80`、M=0、PT=96，且 `size > 12` 与 `size - 12 <= 1440`。

### Heartbeat / HeartbeatAck

```text
byte 0      : type = 1 / 2
byte 1..4   : session_id (u32 LE)
```

长度必须严格等于 5 bytes。client→server 只有这一种包：首包建立 association，
之后只做 NAT/endpoint 续命（单向，无 ACK）。

## 5. 存活分层：association、heartbeat 与 session 超时

三层各管一摊，互不代理：

```text
gRPC keepalive (time 10s / timeout 5s)  → session/控制面存活
UDP heartbeat (首包建连 + 5s 续命)       → association 建立与 UDP 路径存活
                                          （NAT 映射 + endpoint/last_seen 续命）
```

默认：

```text
HELLO_INTERVAL = 1000 ms          # 握手期节奏（association 建立前；建立后转 5s 节奏）
HEARTBEAT_INTERVAL = 5000 ms      # 续命节奏（activity-aware：距上次
                                  # client→server 发包不足一周期则跳过；
                                  # 下游音频不抑制——下行包续不了上行 NAT）
SESSION_TIMEOUT = 30000 ms        # 只看 heartbeat 的 last_seen（≥6× 间隔）
REAP_INTERVAL = 1000 ms
HELLO_ACK_MISS_THRESHOLD = 3      # 仅握手期有效
```

Server 收到 heartbeat：

1. decode（失败 → `malformed_datagrams`）；
2. 必须是 heartbeat（其它类型 → `non_heartbeat_datagrams`）；
3. `SessionManager::establish_session(session_id, sender)`（Created→Connected 与
   已握手刷新同一入口；不存在 → `heartbeat_rejected`）；
4. 首包建连时 reply HeartbeatAck（`heartbeat_ack_attempts` 计的是入队尝试，
   发送本身是 fire-and-forget）；续命包不回 ACK。
5. 每次合法包都更新 endpoint 和 last_seen（漫游/NAT-rebind 续命）。

heartbeat 被拒（`heartbeat_rejected`）只有两种原因：

- sender endpoint 的 port 为 0，或地址是 wildcard（不可回送）；
- session_id 不存在。

**last_seen 只由 heartbeat 刷新；Audio datagram 不更新。**

Client 存活语义：握手期（首个有效 ACK 前）沿用旧规则——收到 ack 则 miss counter 清零，
连续 3 次 miss 触发 liveness failure → association 未建立即 Degraded（连不上 server UDP
端口，重试无意义）。association 一旦建立（首个有效 ACK），定时器转 heartbeat 节奏，
miss 计数冻结；此后 UDP 路径失败只是诊断（计数器照常），**不再导致 session 死亡**，
session 存活只由 gRPC keepalive 判定。

### Client UDP endpoint discovery

客户端在数据面**不要求** HeartbeatAck 的来源 endpoint 与 gRPC 通告的 server endpoint 一致（IPv6 隐私扩展 / 多地址服务器下，ACK 源地址可与 gRPC 地址不同）：

```text
HeartbeatAck:
    校验 session_id == 当前会话（不校验来源地址）
    通过 → learned_endpoint = sender（学习实际对端；同一会话只会有一次建连 ACK）

Audio:
    learned_endpoint 为空（尚未握手）→ 丢弃
    sender == learned_endpoint           → 接受
    sender 不符但 SSRC 命中已钉住流      → 重锁 learned_endpoint 并接受
                                           （server 上游重定向：IPv6 临时地址
                                           轮换/网卡/VPN 抖动）
    否则（陌生源/SSRC 不对）             → 丢弃
```

语义边界：

- session_id 负责会话身份，learned_endpoint 负责 UDP 来源约束，
  SSRC 负责流身份（server 重定向时凭它重锁）；
- 首个有效 HeartbeatAck 确定 association；server 端漫游由 heartbeat 续命覆盖，
  client 不再通过 ACK 重锁（重连即新 session、新握手）；
- Audio 帧不携带 session_id：来源匹配或 SSRC 命中二者居一即接受。

### 两个 endpoint：advertised vs learned

客户端对外暴露两套 endpoint，命名分别对应「预期拨号目标」与「实际数据来源」：

```text
advertised_udp_address/port  gRPC ConnectResponse 通告的 UDP 端点
                             （wildcard 时已 fallback 到 gRPC 连接的 server IP）
learned_udp_address/port     首个有效 HeartbeatAck 的 sender；建连后不再刷新
```

- `advertised_*` 是连接建立时一次写入的拨号目标，之后不再变化；
- `learned_*` 是数据面握手后持续刷新的实际对端（每条有效 ACK 重锁），**不是**一次性初始化参数；
- 上层展示「数据源」时优先显示 `learned_*`，尚未学到（握手前）回退 `advertised_*`。

归属边界：C++ `grpc::ConnectResult` 只携带 `advertised_udp_address/port`（控制面能确定的信息）；`learned_*` 是数据面运行时
状态，由 C API `aqua_connect_result_t` 在 `aqua_client_get_connect_result` 时从 `UdpClient::learned_peer_endpoint()`
动态采样（跨线程读，见 `modules/udp.md`）。


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

## 8. Subscribe（server → client 单向事件）

`Subscribe(session_id)` 建连后 client 应立即订阅；平时阻塞无消息。server 停止时
`stop_locked` 首先广播 Shutdown 事件（不等送达，best-effort），随后才 teardown gRPC：

- 事件赢了竞态 → client 收到明确 `shutdown{reason}`；
- 输了（流被 teardown 中断）→ client 读流失败，行为一致：直接停止退出。

session 不存在订阅立即 NOT_FOUND 结束流；client 对“事件”和“中断”不区分处理，
一律视为 server 不可用。流中断不重试、不重建（重建没有意义：server 已经或即将消失）。

## 9. Trust model

当前协议没有认证：

- gRPC 使用 `InsecureChannelCredentials`，明文、无鉴权；
- heartbeat 只携带 session_id，没有 token。知道一个合法 session_id 的主机可以伪造 heartbeat 覆盖 endpoint（劫持音频流）；
- Audio 帧不携带 session_id，服务端对音频来源**不做任何校验**——任何知道服务端 UDP 端口的主机都可以注入音频源。client 侧的
  唯一约束是来源必须等于 `learned_endpoint`。

因此当前实现适合可信内网/实验环境。公网部署不能直接视为安全协议；未来应在 ConnectResponse 增加随机 token，并把 token 纳入
heartbeat（乃至 Audio）校验。详见 `security_and_deployment.md`。
