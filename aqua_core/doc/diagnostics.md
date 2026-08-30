# Diagnostics

## 1. 两类指标

### Source
返回一个字符串快照，例如：

```text
state{running}
net{rx=..., tx=...}
jb{water=..., used=...}
```

### Counter
Counter 会在每次 snapshot 时计算：

```text
total=累计值
delta=距离上次快照的增量
rate=/s
```

rate 使用真实的 steady_clock elapsed，不假设 timer 绝对精确。

## 2. Debug gating

`Diagnostics::log_debug()` 先判断 Debug 是否启用；未启用时连 source 都不调用。这一点很重要：诊断 getter 本身可能跨多个 atomic 读取，如果用户不看 debug 日志就不应该为它付成本。

## 3. CLI 诊断节奏

CLI main 使用 1s diagnostics timer。额外有 500ms control poll：检测 runtime 是否进入 `Degraded`，若是则主动 stop + `io_context.stop()`。

## 4. Client 关键指标

包括：

- UDP rx/tx packets/bytes/error/drop
- HELLO ack count/miss/age
- malformed / wrong-session / unexpected-sender
- JitterBuffer water / used / capacity
- push accepted/rejected + late/busy/invalid/sanity
- pull calls/frames/silence
- Fill/Drop episodes 与 skipped slots
- reanchor request/cancel/apply/sanity reject
- playback callback pull 统计

## 5. Server 关键指标

包括：

- capture events / packet queries / packets ready / GetBuffer
- capture silent/synthetic silence/starved
- packetizer input blocks/bytes/frames/un-aligned
- handoff queue accept/consume/drop
- dispatcher wakeups / encode / broadcast / no-clients / failures
- UDP rx/tx/drop/errors
- HELLO established/refreshed/rejected
- Session created/connected/refreshed/removed/expired

## 6. RT debug 日志

`AQUA_JITTER_BUFFER_RT_DEBUG_LOG` 默认关闭。Debug 预设会显式开启它，Release 预设保持关闭。该开关会在 `pull()/decide()` 中直接同步调用 spdlog，因此**开启后不再满足严格 realtime logging contract**；仅用于短时间问题复现，不得作为生产默认策略。
