# 流程模型

端到端时序：连接建立、稳态、故障与关闭。状态机与 stop 顺序见 `threading_and_lifecycle.md`。

## 1. 连接建立

```text
Client                              Server
   │── gRPC Connect(client_name) ───►│  创建 Session(Created)，返回
   │                                 │  session_id + udp{address,port} + format + F
   │◄── ConnectResponse ─────────────│
   │  校验格式/F/端口；wildcard 地址回退到 gRPC 的 server_ip
   │                                 │
   │── UDP HELLO(session_id) ───────►│  establish_session：记 NAT endpoint、
   │◄── UDP HELLO_ACK ───────────────│  置 Connected、刷新 last_seen
   │                                 │
   │◄════ UDP Audio datagrams ═══════│  之后持续广播到 Connected session
```

要点：

- gRPC 只建/删 session 与下发参数；保活与音频都在 UDP 数据面。
- HELLO 之前 session 是 `Created`，没有可信 UDP endpoint，server 不会向它广播。
- client 的 UDP remote 取自 ConnectResponse；address 是 wildcard 时回退到 gRPC 连接用的 `server_ip`（端口仍用 response
  端口）。

## 2. 稳态

```text
Server: capture → packetizer → SPSC queue → dispatcher → UDP broadcast
Client: UDP receive → JitterBuffer::push ; playback callback → JitterBuffer::pull

HELLO 每 1s 一次（保活 + 刷 NAT + 刷 last_seen）
Server reaper 每 1s 扫一次，删除 last_seen 超过 5s 的 session
```

只有 HELLO 更新 `last_seen`；Audio datagram 不更新（见 `protocol.md` §5）。

## 3. 故障路径

| 事件                                | 检测者                            | 结果                                                      |
|-------------------------------------|-----------------------------------|-----------------------------------------------------------|
| 设备断开 / 音频服务异常             | capture/playback `event_callback` | runtime 置 `Degraded`，CLI control poll（500ms）stop+exit |
| HELLO_ACK 连续 3 次 miss            | client HELLO timer                | liveness failure → `Degraded`                             |
| session 超时（5s 无 HELLO）         | server reaper                     | `remove_expired_sessions`                                 |
| loopback quiescence（静音无 event） | capture 20ms 超时探测             | 合成静音保时间轴（**不是错误**）                          |
| 畸形 / 错误 payload                 | UDP 解码校验                      | 统计 + 丢弃，不终止接收循环                               |

`Degraded` 是「一次性终态」：没有回 `Running` 的路径，由上层（CLI）负责停进程。将来若要自动重连，需要新状态机 + 恢复路径（当前明确不做）。

## 4. 关闭

```text
Client: playback.stop() → udp.stop() → gRPC Disconnect(best-effort)
Server: capture.stop() → cancel reaper → dispatcher.stop()+join
        → udp.stop() → grpc.shutdown()+join → sessions.clear()
```

原则：先停消费端/生产端，再停网络，最后清控制面，避免 teardown 与 RT callback 交叠（见 `threading_and_lifecycle.md` §6）。
