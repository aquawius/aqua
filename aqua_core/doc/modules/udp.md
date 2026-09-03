# 模块：UDP

## NetworkFrame

`NetworkFrame`（`net/udp/network_frame.*`）是纯 wire codec：

```text
PacketType + sequence / session_id + payload
```

- 全部手工小端编解码，不依赖主机字节序；
- `PacketType`：`Invalid=0` / `Hello=1` / `HelloAck=2` / `Audio=3`；
- decode 得到的是借用视图，只在输入 datagram buffer 存活期间有效；
- 未知 type 返回 `nullopt`。

字段布局与校验规则见 `../protocol.md` §4。

## UdpTransport

底层 socket + strand + 有界发送队列，统一处理 bind/open、remote、异步收发、socket 缓冲、stop 与计数。它不知道 session、
HELLO、AudioFrame、PCM 或 JitterBuffer。细节见 `udp_transport.md`。

## UdpServer

只处理 HELLO：其余 packet type 一律计入 `non_hello_datagrams`。广播时从 `SessionManager` 快照所有 Connected endpoint，把
同一个 immutable 编码后 datagram 共享发往多个 endpoint（一次编码，多份发送）。

| 计数器                  | 含义                                                             |
|-------------------------|--------------------------------------------------------------------|
| `malformed_datagrams`   | 解码失败                                                            |
| `non_hello_datagrams`   | 合法帧但类型不是 HELLO（**Audio 与 HelloAck 都归在此类**）           |
| `hello_received`        | 收到 HELLO                                                          |
| `hello_rejected`        | HELLO 被拒（endpoint 不可用 / session 不存在）                      |
| `sessions_established`  | 首次握手成功（Created → Connected）                                 |
| `sessions_refreshed`    | 已 Connected 的 session 再次 HELLO                                  |
| `hello_ack_attempts`    | HELLO_ACK 入队尝试次数（fire-and-forget，队列溢出被丢也计数）        |

## UdpClient

启动接收时指定 expected audio payload bytes（`F × frame_bytes`）；只有 payload 长度**严格等于**该值且来源匹配的 Audio 包才会
交给回调。HELLO timer 每秒运行并维护 ack miss 状态。

接收分类顺序与计数器：

| 计数器                        | 触发条件                                                     |
|-------------------------------|----------------------------------------------------------------|
| `malformed_datagrams`         | 解码失败                                                       |
| `wrong_session_acks`          | HelloAck 的 session_id 与当前会话不符（或为 0）                |
| `non_audio_datagrams`         | 合法帧但不是 Audio（HelloAck 已内部消化，故实际是 Hello 等）   |
| `unexpected_sender_datagrams` | Audio 的 sender ≠ `learned_endpoint`                           |
| `audio_payload_mismatches`    | Audio 的 payload 长度 ≠ expected                               |
| `audio_frames_accepted`       | 通过全部校验并交给回调                                         |
| `hello_send_attempts`         | HELLO 发送尝试总数（首次发送 + 周期重发均计数）                 |

`learned_endpoint`（HELLO_ACK 的实际来源）由接收 handler 在 io 线程写、查询方（C API）读，用 `learned_mutex_` 保护短临界区；
查询入口 `learned_peer_endpoint()` 返回 `std::optional<endpoint>`（未握手时为 `nullopt`）。握手完成前的 Audio 一律丢弃。

## 三层"缓冲"不要混淆

```text
kernel SO_RCVBUF / SO_SNDBUF（各 64 KiB）  吸收 OS / 网络突发
UdpTransport 应用发送队列（64 datagrams）  控制 async send 积压
JitterBuffer                                播放时间线缓冲
```
