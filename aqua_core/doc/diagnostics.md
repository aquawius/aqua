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

`Diagnostics::log_debug()` 先判断 Debug 是否启用；未启用时连 source 都不调用。这一点很重要：诊断 getter 本身可能跨多个
atomic 读取，如果用户不看 debug 日志就不应该为它付成本。

RT 路径的调试日志开关 `AQUA_JB_RUNTIME_THREAD_DEBUG_LOG`（默认关；Debug 构建预设显式开启、Release 预设关闭）一旦开启会在 `pull()/decide()` 与音频回调中同步调用 spdlog，**破坏严格 RT 契约**，仅用于短时间问题复现。

决策层的调试日志开关 `AQUA_JB_CONTROL_THREAD_DEBUG_LOG`（同样默认关、debug 预设开）覆盖 estimator / controller /
push strand 的判定日志。它运行在非实时线程上，不影响音频实时性，但会给每包处理路径加上带锁的格式化，同样只用于短时间
排查。两个宏的边界、点位全表与"看到某行日志该去哪查"的排查表见 `modules/observability.md`。

## 3. CLI 诊断节奏

CLI main 使用 1s diagnostics timer。额外有 500ms control poll：检测 runtime 是否进入 `Degraded`，若是则主动 stop +
`io_context.stop()`。

## 4. Client 关键指标

包括：

- UDP rx/tx packets/bytes/error/drop
- Heartbeat ack count/miss/age
- malformed / wrong-session / unexpected-sender
- JitterBuffer water / used / capacity
- push accepted/rejected + late/busy/invalid/sanity
- pull calls/frames/silence
- Fill/Drop episodes 与 skipped slots
- reanchor request/cancel/apply/sanity reject
- playback callback pull 统计

### 4.1 决策层观测（`jitter_control` 组）

上面那批是"结果与累计计数"；这一组是"决策与阈值"，回答**为什么** target / 水位 / 掩盖是现在这样。三部分来源不同：

| 来源 | 字段 | 用途 |
|------|------|------|
| TargetController（决策层，仅自适应模式） | `adaptive`、`desired_slots`、`min_slots`、`max_slots`、`margin_source`、`path`、`floor_bound`、`cap_bound`、`underrun_penalty`、`dwell_remaining_ms`、`fall_room_slots` | target 为什么涨 / 跌 / 不动 |
| JitterEstimator（观测层尾部） | `stall_events`、`stall_peak_ms`、`last_stall_gap_ms`、`arrival_interval_ms` | 断流侧：门剔除次数与近期最坏间隙 |
| JitterBuffer（执行层） | `band_warning_low`/`band_normal_low`/`band_normal_high`/`band_warning_high`、`conceal_run_slots`、`underrun_run_slots` | 阈值与"此刻在发生什么" |

判读入口（三个最常用的组合）：

- `desired_slots != target_slots` → 本拍 target 被限速 / 涨后锁跌 / 死区按住，看 `path` 与 `dwell_remaining_ms`；
- `floor_bound=1` → 算出来的余量不够、靠下限（几何地板或欠载反馈）托住；`cap_bound=1` → 顶到 2/3 结构上限；
- `stall_peak_ms` 大而 `estimator_jitter_ms` 小 → 缺口是"断流尾部被门剔除"，该调 stall 峰值上限而不是 k。

字段语义与调整方向见 `jitter_buffer_control_design.md` §5；日志侧的对应点位见 `modules/observability.md`。
Android 侧这一组经 `aqua_jitter_control_stats_t` 下发，主页"自适应缓冲 / 网络抖动 / 音质"三张卡就是它的展示。

## 5. Server 关键指标

包括：

- capture events / packet queries / packets ready / GetBuffer
- capture silent / synthetic silence / starved
- packetizer input blocks/bytes/frames/un-aligned
- handoff queue accept/consume/drop
- dispatcher wakeups / encode / broadcast / no-clients / failures
- UDP rx/tx/drop/errors
- Heartbeat established/refreshed/rejected
- Session created/connected/refreshed/removed/expired
- **capture_switch**：state / route / active_device_id / requested_device_id / last_outcome / last_switch_error

`capture_switch` 是管理级状态，与流级的 `capture.state`（active/silent/starved）正交：前者讲设备切换事务，后者讲采集时间轴。
排障设备问题时以前者为准，见 `operations_and_troubleshooting.md` §6。

## 6. 快照口径

`ServerDiagnosticsSnapshot` / `ClientDiagnosticsSnapshot` 是一次性聚合：各字段为独立的原子近似读，跨字段不保证同一时刻一致，
仅供监控与显示，**不用于控制决策**。

两处容易误读的口径：

- `dispatcher.dropped_frames` 转发的是 `AudioFrameQueue` 的丢弃数，dispatcher 自身没有丢弃计数器；
- session 的 `removed` 已包含 `expired` 与 `removed_by_clear`，三者不能相加求总数。

