# 模块：SessionManager

## 数据

每个 session：

```text
session_id
endpoint
created_at
last_seen
state = Created | Connected
```

只有 Connected session 才能被 UDP broadcast 选中。

## 并发

`std::shared_mutex` 保护 map：

- create/remove/establish/touch/clear：unique lock；
- get/is_connected/snapshot/count：shared lock。

Stats 用 atomic，不需要和 map 共用锁做统计读取。

## 存活写入分工

- `on_heartbeat`：返回 `HeartbeatOutcome` 三态。`Established`（首包， Created→Connected）记 endpoint 并刷新 last_seen（桥接
  Connect 到首次 Keepalive 的空窗）；`Refreshed`（续命包）只覆盖 endpoint（NAT 重绑/漫游 静默跟随），不碰 last_seen；非
  `Rejected` 的包 server 都回 HeartbeatAck；
  `Rejected`（非法 endpoint 或未知 session）无副作用；
- `touch_session_liveness`：proto Keepalive 的唯一 last_seen 写入口， session 不存在返回 false（调用方应停止，而非重试）。

## ID

u32 session id 由 CSPRNG（`std::random_device`，Windows=BCryptGenRandom / Linux/Android=/dev/urandom）每会话独立生成；0
保留无效。创建时检查 collision，理论耗尽则失败。session_id 是 HeartbeatAck 阶段唯一的身份凭据，必须不可预测（旧实现是随机
instance_id + 自增 counter，观察者可推断后续 id，已废弃）。

## Endpoint 的权威来源

Connect 只产生 session id。真正可发送的 UDP endpoint 来自该 session 最近一次 heartbeat 的 sender address/port。

因此 server 不相信 client 自己声称的 UDP 来源；以网络包实际 sender endpoint 为准。

## 超时

`remove_expired_sessions(timeout)` 在同一把 unique lock 内完成检查和删除，避免扫描后再次判断造成 TOCTOU。判定条件为
`now - last_seen > timeout`（默认 `SESSION_TIMEOUT = 5000ms`，reaper 每 `REAP_INTERVAL = 1000ms` 跑一次）。

只有 proto Keepalive（+ 建连跃迁）会刷新 `last_seen`；heartbeat 只刷新 endpoint（漫游续命），Audio datagram 不刷新。

## Stats

六个计数器，读取不需要取 map 的锁：

| 字段               | 含义                                                    |
|--------------------|---------------------------------------------------------|
| `created`          | `create_session` 成功次数                               |
| `connected`        | 首次握手成功（Created → Connected）                     |
| `refreshed`        | 已 Connected 的 session 续命 heartbeat                  |
| `removed`          | 删除成功次数（**过期删除也会自增，与 `expired` 重叠**） |
| `expired`          | 因过期被删的次数                                        |
| `removed_by_clear` | `clear()` 批量删除的数量（同时也计入 `removed`）        |

因此这些计数 **不能相加求总数**：`removed` 已经包含 `expired` 与 `removed_by_clear`。
