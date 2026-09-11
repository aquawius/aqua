# 模块：UDP

## NetworkFrame

`NetworkFrame`（`net/udp/network_frame.*`）是纯 wire codec：

```text
PacketType + sequence / session_id + payload
```

- 按包型分区字节序：Heartbeat/HeartbeatAck 手工小端（5B），Audio RTP 头 12B
  大端（PT=96，Wireshark 可解析），都不依赖主机字节序；
- `PacketType`：`Invalid=0` / `Heartbeat=1` / `HeartbeatAck=2` / `Audio=3`；
- decode 得到的是借用视图，只在输入 datagram buffer 存活期间有效；
- 未知 type 返回 `nullopt`。

字段布局与校验规则见 `../protocol.md` §4。

## UdpTransport

底层 socket + strand + 有界发送队列，统一处理 bind/open、remote、异步收发、socket 缓冲、stop 与计数。它不知道 session、
Heartbeat、AudioFrame、PCM 或 JitterBuffer。细节见 `udp_transport.md`。

## UdpServer

只处理 heartbeat：其余 packet type 一律计入 `non_heartbeat_datagrams`。广播时从 `SessionManager` 快照所有 Connected endpoint，把
同一个 immutable 编码后 datagram 共享发往多个 endpoint（一次编码，多份发送）。

| 计数器                  | 含义                                                             |
|-------------------------|--------------------------------------------------------------------|
| `malformed_datagrams`   | 解码失败                                                            |
| `non_heartbeat_datagrams` | 合法帧但类型不是 heartbeat（实际是 Audio，直丢）                     |
| `heartbeat_received`    | 收到 heartbeat（首包建连 + 后续续命）                                |
| `heartbeat_rejected`    | heartbeat 被拒（endpoint 不可用 / session 不存在）                   |
| `sessions_established`  | 首次握手成功（Created → Connected）                                 |
| `sessions_refreshed`    | 已 Connected 的 session 续命 heartbeat                              |
| `heartbeat_ack_attempts`| HeartbeatAck 入队尝试次数（fire-and-forget，每个合法 heartbeat 都回）  |
| `heartbeat_received`    | 收到 heartbeat（合法 session 才续命，未知/未握手计 rejected）        |
| `heartbeat_rejected`    | heartbeat 被拒（session 不存在或未握手）                            |

## UdpClient

启动接收时指定 expected audio payload bytes（`F × frame_bytes`）；只有 payload 长度**严格等于**该值、来源匹配且
SSRC 钉住一致的 Audio 包才会交给回调。单一定时器按 phase 定节奏：握手期按 handshake_interval 等 ACK（连续 3 miss 即建连失败），建连后转 1s 稳态节奏并继续 ACK 跟踪（连续 5 miss 即路径死亡，经 LivenessHandler 上报）。

接收分类顺序与计数器：

| 计数器                        | 触发条件                                                     |
|-------------------------------|----------------------------------------------------------------|
| `malformed_datagrams`         | 解码失败                                                       |
| `wrong_session_acks`          | HeartbeatAck 的 session_id 与当前会话不符（或为 0）            |
| `non_audio_datagrams`         | 合法帧但不是 Audio（HeartbeatAck 已内部消化）                  |
| `unexpected_sender_datagrams` | Audio 的 sender ≠ `learned_endpoint` 且 SSRC 未命中已钉住流 |
| `audio_payload_mismatches`    | Audio 的 payload 长度 ≠ expected                               |
| `audio_frames_accepted`       | 通过全部校验并交给回调                                         |
| `heartbeat_handshake_send_attempts`         | 握手期 heartbeat 发送总数（建连后冻结，不含续命包）            |

`learned_endpoint`（HeartbeatAck 的实际来源）由接收 handler 在 io 线程写、查询方（C API）读，用 `learned_mutex_` 保护短临界区；
查询入口 `learned_peer_endpoint()` 返回 `std::optional<endpoint>`（未握手时为 `nullopt`）。握手完成前的 Audio 一律丢弃。

## 三层"缓冲"不要混淆

```text
kernel SO_RCVBUF / SO_SNDBUF（各 64 KiB）  吸收 OS / 网络突发
UdpTransport 应用发送队列（64 datagrams）  控制 async send 积压
JitterBuffer                                播放时间线缓冲
```
