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

CLI 侧这套 `total/delta/rate` 由 `Diagnostics::log_debug()` 维护。 **Android 侧等价逻辑在 Kotlin 层**（`AquaRates.kt` 的
`RateSampler`）： 诊断快照本身仍是"无时间状态的聚合快照"（契约不变、槽位不变），App 对相邻两次采样做差分、除以真实 elapsed 得到
/s， 主页卡片把它作为累计值下方的一行展示——累计值看不出"此刻是否在恶化"，速率才看得出来。两处只在 **拿到新诊断**时采样
（Android 侧诊断刷新约 1s 一次），计数器回退（重连后从 0 重计）时该拍跳过，避免算出负速率。

## 2. Debug gating

`Diagnostics::log_debug()` 先判断 Debug 是否启用；未启用时连 source 都不调用。这一点很重要：诊断 getter 本身可能跨多个
atomic 读取，如果用户不看 debug 日志就不应该为它付成本。

RT 路径的调试日志开关 `AQUA_JB_RUNTIME_THREAD_DEBUG_LOG`（默认关；Debug 构建预设显式开启、Release 预设关闭）一旦开启会在
`pull()/decide()` 与音频回调中同步调用 spdlog， **破坏严格 RT 契约**，仅用于短时间问题复现。

决策层的调试日志开关 `AQUA_JB_CONTROL_THREAD_DEBUG_LOG`（同样默认关、debug 预设开）覆盖 estimator / controller / push
strand 的判定日志。它运行在非实时线程上，不影响音频实时性，但会给每包处理路径加上带锁的格式化，同样只用于短时间
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

上面那批是"结果与累计计数"；这一组是"决策与阈值"，回答 **为什么** target / 水位 / 掩盖是现在这样。三部分来源不同：

| 来源                                     | 字段                                                                                                                                                                    | 用途                             |
|------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------|
| TargetController（决策层，仅自适应模式） | `adaptive`、`desired_slots`、`min_slots`、`max_slots`、`margin_source`、`path`、`floor_bound`、`cap_bound`、`underrun_penalty`、`dwell_remaining_ms`、`fall_room_slots` | target 为什么涨 / 跌 / 不动      |
| JitterEstimator（观测层尾部）            | `stall_events`、`stall_peak_ms`、`last_stall_gap_ms`、`arrival_interval_ms`                                                                                             | 断流侧：门剔除次数与近期最坏间隙 |
| JitterBuffer（执行层）                   | `band_warning_low`/`band_normal_low`/`band_normal_high`/`band_warning_high`、`conceal_run_slots`、`underrun_run_slots`                                                  | 阈值与"此刻在发生什么"           |

判读入口（三个最常用的组合）：

- `desired_slots != target_slots` → 本拍 target 被限速 / 涨后锁跌 / 死区按住，看 `path` 与 `dwell_remaining_ms`；
- `floor_bound=1` → 算出来的余量不够、靠下限（几何地板或欠载反馈）托住；`cap_bound=1` → 顶到 2/3 结构上限；
- `stall_peak_ms` 大而 `estimator_jitter_ms` 小 → 缺口是"断流尾部被门剔除"，该调 stall 峰值上限而不是 k。

字段语义与调整方向见 `jitter_buffer_control_design.md` §5；日志侧的对应点位见 `modules/observability.md`。 Android 侧这一组经
`aqua_jitter_control_stats_t` 下发。主页按 **模块**分卡（每张卡只放自己模块的量，不跨模块借字段）：

| 卡片       | 归属模块             | 取自本组的字段                                                                                                                         |
|------------|----------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| 连接       | heartbeat / session  | —                                                                                                                                      |
| 传输       | UDP 数据面           | —                                                                                                                                      |
| 网络抖动   | JitterEstimator      | `stall_events` / `stall_peak_ms` / `last_stall_gap_ms` / `arrival_interval_ms`                                                         |
| 缓冲       | JitterBuffer（机械） | `band_*`                                                                                                                               |
| 自适应缓冲 | TargetController     | `adaptive` / `desired_slots` / `min_slots` / `max_slots` / `margin_source` / `path` / `floor_bound` / `cap_bound` / `underrun_penalty` |
| 播放输出   | 播放后端 + JB 输出   | —（用 JB 的 `pull_*` 与 `consecutive_silence_frames`）                                                                                 |
| 音质       | JB 听感结果          | `conceal_run_slots` / `underrun_run_slots`                                                                                             |

两处口径容易看混，卡片已刻意分开：

- **静音**只出现在「播放输出」（`pull_silence_frames`，时间轴修正吐出来的静音）， **不**
  出现在「音质」——音质只统计"没数据可用"的欠载与掩盖；
- **`pull_*` 与 playback 的 `pull_*` 是同一批 pull 在两个层次各记一次**（JB 在 `pull()` 内自记，ClientRuntime
  在调用处再记），数值相同，UI 统一用 JB 侧那一份。

### 4.2 播放设备切换的提示判据（Android）

"设备真的换了吗"要看 **事务序号**而不是结果枚举：

- `switch_seq`：每笔切换事务（成功 / 回滚 / 兜底 / 链耗尽）递增一次，是 UI 提示的 **主判据**。outcome + error
  只能表达"最近一次结果"，两笔不同的事务完全可能 给出相同的值——典型的"手动 pin 蓝牙"与"蓝牙断开后自动回落扬声器"都是
  `Switched + None`，只看枚举会把后一笔整个吞掉（V0.2.2 起"回落了但不提示"
  的成因）；
- `requested_device_id` / `stream_device_id` 查询对里的 **实际输出设备**变化作为 **辅助判据**：覆盖"core 没发起事务、Android
  自己把流重路由了"的情况。部分平台 （AAudio `getDeviceId` 返回 UNSPECIFIED）回读为空，因此不能当唯一判据。

同一拍只提示一次（事务序号优先）；显式切换（`setPlaybackDevice`）路径会预置序号 锁存，避免同一件事弹两次横幅。core
侧不维护"提示过没有"这类 UI 状态。

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
仅供监控与显示， **不用于控制决策**。

两处容易误读的口径：

- `dispatcher.dropped_frames` 转发的是 `AudioFrameQueue` 的丢弃数，dispatcher 自身没有丢弃计数器；
- session 的 `removed` 已包含 `expired` 与 `removed_by_clear`，三者不能相加求总数。

