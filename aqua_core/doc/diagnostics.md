# Diagnostics & Logging

## 1. 原则

Diagnostics 只能回答运行状态，不参与 correctness 控制。

`Diagnostics` 当前不是 thread-safe；注册 source/counter 与采样必须由同一 control thread/等价串行上下文完成。

## 2. Server 关键指标

- capture blocks / bytes
- packetizer frames / unaligned input
- queue accepted / consumed / dropped / depth
- dispatcher published / wakeups / encoded / broadcast / no-clients
- UDP rx/tx/errors/drops/enqueue-failures
- HELLO received / rejected / sessions established / refreshed
- HELLO_ACK send attempts
- session counts / expired / removed

## 3. Client 关键指标

- UDP rx/tx/errors/drops
- HELLO send attempts / ACK count / consecutive misses / total miss events
- unexpected sender / wrong session ACK / malformed / payload mismatch
- JitterBuffer water/used/capacity
- push accepted/rejected/late/busy/invalid/sanity
- pull frames/silence
- fill/drop/reanchor statistics
- playback callback counts

## 4. Counter terminology

带 `*_attempts` 的计数器表示“尝试提交/发起”，不是网络层已经实际送达。例如：

```text
hello_send_attempts
hello_ack_attempts
```

不能解释成对端已收到。

## 5. JitterBuffer RT debug logging

源码提供：

```text
AQUA_JITTER_BUFFER_RT_DEBUG_LOG
```

默认关闭。

开启后允许 JitterBuffer `pull()/decide()` 内输出内部 fill/drop/reanchor 日志，用于离线问题定位。该开关明确违反正常 RT no-lock/no-alloc/no-sync-I/O contract，不得用于性能基准或常规 release 运行。

CMake：

```text
-DAQUA_JITTER_BUFFER_RT_DEBUG_LOG=ON
```
