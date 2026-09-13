# CLI 诊断

## Server 每 1 秒 Debug snapshot

核心 source：

```text
state
 audio
 queue
 sessions
 udp
```

counter 覆盖 capture、packetizer、queue、dispatcher、UDP、session。

## Client 每 1 秒 Debug snapshot

核心 source/counter 覆盖：

```text
state
 grpc result
 udp stats + heartbeat liveness
 jitter water/used/reanchor
 jitter push/pull/fill/drop
 jitter control (jc)    # TargetController 决策细节：target 为什么是这个值、被什么夹住（仅 --jb-adaptive-target 自适应模式有意义）
 playback pull
```

> **`jc`（jitter control）源**：输出 TargetController 当期结算——`adaptive / desired / min / geo_floor`（含 `ms`）、`margin`
> 的胜出方 `src`（`kJ` 还是 `stall_peak`）、收敛路径 `path`、是否被 `floor_bind` / `cap_bind` 夹住、欠载惩罚 `penalty`、涨后锁跌剩余
> `dwell`、跌侧限速额度 `fall_room`，以及 stall 侧与 `bands` 五档水位带、conceal/underrun 计数。只有开启自适应 target
> 时才有内容；固定模式（`--jb-fixed-target`）下该源为空。注册点位见 `aqua_app/cli/client_main.cpp` 的
> `diag.add_source("jc", ...)`。

## 控制轮询

Server/Client 还有 500ms control poll。它不是音频控制器，只负责观察 runtime 是否进入 `Degraded` 等 terminal condition。

因此“500ms poll”不能解释成“500ms 一次播放调整”。JitterBuffer 完全由 playback callback 驱动。
