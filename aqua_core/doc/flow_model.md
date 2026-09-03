# 流程模型

端到端时序：连接建立、稳态、故障与关闭。状态机与 stop 顺序见 `threading_and_lifecycle.md`。

## 1. 连接建立

```text
Client                              Server
   │── gRPC Connect(client_name) ───►│  创建 Session(Created)，返回
   │                                 │  session_id + udp{address,port} + format + F
   │◄── ConnectResponse ─────────────│
   │  校验格式 / F / 端口；地址是 wildcard 时回退到 gRPC 连接用的 server_ip
   │                                 │
   │── UDP HELLO(session_id) ───────►│  establish_session：记 NAT endpoint、
   │◄── UDP HELLO_ACK ───────────────│  置 Connected、刷新 last_seen
   │                                 │
   │◄════ UDP Audio datagrams ═══════│  之后持续广播到 Connected session
```

要点：

- gRPC 只建/删 session 并下发参数；保活与音频都在 UDP 数据面。
- HELLO 之前 session 处于 `Created`，没有可信 UDP endpoint，server 不会向它广播。
- client 的 UDP remote 取自 ConnectResponse；address 为 wildcard 时回退到 gRPC 的 `server_ip`（端口仍用响应中的端口）。

## 2. 稳态

```text
Server: capture → packetizer → SPSC queue → dispatcher → UDP broadcast
Client: UDP receive → JitterBuffer::push ; playback 回调 → JitterBuffer::pull

HELLO 每 1s 一次（保活 + 刷 NAT 映射 + 刷 last_seen）
Server reaper 每 1s 扫一次，删除 last_seen 超过 5s 的 session
control timer 每 500ms 一次：server 检查 capture 切换，client 检查 playback 恢复与默认设备跟随
```

只有 HELLO 更新 `last_seen`；Audio datagram 不更新（见 `protocol.md` §5）。

## 3. 设备故障与切换

设备故障不再是终止事件，而是一次会话内的端点重建。两条驱动路径：

```text
错误驱动（路径 1）
  backend event 回调（DeviceDisconnected）
    → 置待处理标志（不 stop、不置 Degraded）
    → control tick（500ms）执行 restart 事务

主动跟随（路径 2，仅 FollowSystem）
  管理状态 tick 轮询系统默认设备
    → 与当前实际设备不同 → 执行 restart 事务
```

restart 事务（两侧同构）：

```text
管理状态 = Switching
  捕获 previous_active_device
  stop 旧流（同步 join）
  候选链 [目标设备, 先前的实际设备, 系统默认] 逐个尝试，首个成功即 Running
  链耗尽 → Fatal → CLI 停止会话
```

client 侧间隙由 JitterBuffer 水位机制吸收；server 侧间隙表现为 packet gap，由对岸 client 的 JitterBuffer 饥饿路径吸收。
seq 与会话都不重置。

## 4. 故障路径总表

| 事件                                | 检测者                            | 结果                                                          |
|-------------------------------------|-----------------------------------|---------------------------------------------------------------|
| 设备断开 / 失效                     | capture / playback event 回调     | 走 restart 事务；成功则继续，链耗尽 → Fatal → stop              |
| 切换重试超限（10s 内 3 次）         | `CaptureManager` / `PlaybackManager` | 直接 Fatal，不再触碰后端 → CLI stop                          |
| 非设备的后端错误                    | event 回调                        | 置 `Degraded`，CLI control poll（500ms）stop + exit            |
| HELLO_ACK 连续 3 次 miss            | client HELLO timer                | liveness failure → `Degraded`                                 |
| session 超时（5s 无 HELLO）         | server reaper                     | `remove_expired_sessions`                                     |
| loopback quiescence（无 render client） | capture 20ms 超时探测         | 按墙钟欠账合成静音维持时间轴（**不是错误**）                   |
| 畸形 / 长度不符的 payload           | UDP 解码校验                      | 计数并丢弃，不终止接收循环                                     |

`Degraded` 是一次性终态：没有回到 `Running` 的路径，由上层（CLI）负责停进程。自动重连不在 runtime 状态机内——它属于应用层
策略（Android App 在 Kotlin 层以 3s 退避重连，runtime 不感知）。

## 5. 关闭

```text
Client: playback.stop() → udp.stop() → gRPC Disconnect(best-effort)
Server: capture.stop() → cancel reaper → dispatcher.stop()+join
        → udp.stop() → grpc.shutdown()+join → sessions.clear()
```

原则：先停消费端/生产端，再停网络，最后清控制面，避免 teardown 与 RT 回调交叠（见 `threading_and_lifecycle.md` §7）。
