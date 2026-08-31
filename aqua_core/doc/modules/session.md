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

- create/remove/establish/clear：unique lock；
- get/is_connected/snapshot/count：shared lock。

Stats 用 atomic，不需要和 map 共用锁做统计读取。

## ID

u32 session id 由 CSPRNG（`std::random_device`，Windows=BCryptGenRandom / Linux/Android=/dev/urandom）每会话独立生成；0 保留无效。创建时检查 collision，理论耗尽则失败。session_id 是 HELLO_ACK 阶段唯一的身份凭据，必须不可预测（旧实现是随机 instance_id + 自增 counter，观察者可推断后续 id，已废弃）。

## Endpoint 的权威来源

Connect 只产生 session id。真正可发送的 UDP endpoint 来自该 session 最近一次成功 HELLO 的 sender address/port。

因此 server 不相信 client 自己声称的 UDP 来源；以网络包实际 sender endpoint 为准。

## 超时

`remove_expired_sessions(timeout)` 在同一把 unique lock 内完成检查和删除，避免扫描后再次判断造成 TOCTOU。
