# 协议

## 1. 双平面

Aqua 使用两个逻辑通道：

```text
Control plane  = gRPC TCP
Data plane     = UDP
```

gRPC 做 session 生命周期、音频流参数下发，以及 proto Keepalive 存活判定；不传输音频。

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
之后做 NAT/endpoint 续命；server 对每个合法包都回 HeartbeatAck。

## 5. 存活分层：association、heartbeat、proto keepalive 与 session 超时

三层各管一摊，互不代理：

```text
proto Keepalive (1s 间隔 / 800ms deadline) → session/控制面存活（应用层判定）
UDP heartbeat (首包建连 + 1s 节奏)          → association 建立与 UDP 路径存活
                                            （NAT 映射 + endpoint 续命；不碰 last_seen）
                                            server 每包回 ACK，client 两阶段都跟踪
```

注意：传输层 channel 参数保持默认——存活判定只看应用层结果，从根上杜绝
GOAWAY 误杀那类版本相关调参事故。

默认：

```text
HELLO_INTERVAL = 1000 ms          # 握手期节奏（association 建立前；建立后转 1s 稳态节奏）
HEARTBEAT_INTERVAL = 1000 ms      # 稳态节奏（activity-aware：距上次
                                  # client→server 发包不足一周期则跳过；
                                  # 下游音频不抑制——下行包续不了上行 NAT；
                                  # 跳过的周期不计 miss）
GRPC_KEEPALIVE_INTERVAL = 1000 ms   # proto Keepalive 节奏
GRPC_KEEPALIVE_DEADLINE = 800 ms    # 单次超时；必须 < interval
GRPC_KEEPALIVE_MISS_THRESHOLD = 5 # 传输连续失败阈值；SessionGone 立即，不重试
SESSION_TIMEOUT = 5000 ms         # 只看 proto Keepalive 刷新的 last_seen（5× 间隔）
REAP_INTERVAL = 1000 ms
HELLO_ACK_MISS_THRESHOLD = 3      # 握手期：连续 3 周期无 ACK 即建连失败
HEARTBEAT_ACK_MISS_THRESHOLD = 5  # 稳态：连续 5 周期无 ACK 即路径死亡（与 SESSION_TIMEOUT 对齐）
```

Server 收到 heartbeat：

1. decode（失败 → `malformed_datagrams`）；
2. 必须是 heartbeat（其它类型 → `non_heartbeat_datagrams`）；
3. `SessionManager::on_heartbeat(session_id, sender)` → `HeartbeatOutcome`
  （Established = 首包建连，Refreshed = 续命跟随，Rejected = 未知 session →
   `heartbeat_rejected`）；
4. 每个合法 heartbeat 都 reply HeartbeatAck（`heartbeat_ack_attempts` 计的是入队尝试，
   发送本身是 fire-and-forget）；首包是建连确认，之后是路径探活回执。
5. 每次合法包都更新 endpoint（漫游/NAT-rebind 续命）；**last_seen 不碰**——
   存活是 proto Keepalive 的事，UDP 续命永远续不了已死的控制面。

Server 收到 Keepalive：session 存在即刷新 last_seen 并返回 valid=true；
不存在返回 valid=false（client 应停止，而非重试）。

heartbeat 被拒（`heartbeat_rejected`）只有两种原因：

- sender endpoint 的 port 为 0，或地址是 wildcard（不可回送）；
- session_id 不存在。

**last_seen 只由 proto Keepalive 与建连跃迁刷新；Audio datagram 不更新。**

Client 存活语义（双致命）：握手期（首个有效 ACK 前）收到 ack 则 miss counter 清零，
连续 3 次 miss 触发 liveness failure → Degraded（连不上 server UDP 端口，
重试无意义）。association 一旦建立，转 1s 稳态节奏并继续 ACK 跟踪，连续 5 次
miss（约 5s）同样触发 liveness failure → Degraded。另起一路 proto Keepalive
探活控制面，传输连续失败达阈值（与 UDP 对称）或会话已不在即 Degraded。任一死亡 supervision
观察到后 stop + 退出，不重试。

### Client UDP endpoint discovery

客户端在数据面**不要求** HeartbeatAck 的来源 endpoint 与 gRPC 通告的 server endpoint 一致（IPv6 隐私扩展 / 多地址服务器下，ACK 源地址可与 gRPC 地址不同）：

```text
HeartbeatAck:
    校验 session_id == 当前会话（不校验来源地址）
    通过 → learned_endpoint = sender（学习实际对端；首包确定 association，
    之后每包的 ACK 同样重锁，sender 不变时无效果）

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
learned_udp_address/port     每个有效 HeartbeatAck 的 sender 重锁（首包确定
                             association；之后 sender 通常不变，重锁无效果）
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

## 8. Keepalive（proto 应用层探活）

`Keepalive(session_id)` 由 client 周期调用（`GRPC_KEEPALIVE_INTERVAL`），单次
`GRPC_KEEPALIVE_DEADLINE` 超时；server 存在即刷新 last_seen 并返回 valid=true，
不存在返回 valid=false。client 传输连续失败达 GRPC_KEEPALIVE_MISS_THRESHOLD
（与 UDP 稳态对称，容忍单次抖动）或会话已不在（SessionGone，确定性结论立即）
即 Degraded，随后 supervision 停服——不重试（控制面已死或会话已不在，
重试没有意义）。

选应用层而不调传输层参数的原因：HTTP/2 keepalive 的 GOAWAY 节流是版本相关的
隐式策略，调参埋雷；proto RPC 是普通 data，不受 throttle，用一次 800ms deadline
的失败语义表达存活，行为完全由自己定义。

## 9. Trust model

当前协议没有认证：

- gRPC 使用 `InsecureChannelCredentials`，明文、无鉴权；
- heartbeat 只携带 session_id，没有 token。知道一个合法 session_id 的主机可以伪造 heartbeat 覆盖 endpoint（劫持音频流）；
- Audio 帧不携带 session_id，服务端对音频来源**不做任何校验**——任何知道服务端 UDP 端口的主机都可以注入音频源。client 侧的
  唯一约束是来源必须等于 `learned_endpoint`。

因此当前实现适合可信内网/实验环境。公网部署不能直接视为安全协议；未来应在 ConnectResponse 增加随机 token，并把 token 纳入
heartbeat（乃至 Audio）校验。详见 `security_and_deployment.md`。
