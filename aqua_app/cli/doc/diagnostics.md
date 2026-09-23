# CLI 诊断

CLI 只是诊断的**一个消费者**：每 1 秒取一次聚合快照，交给 `SnapshotLine` 打成一行 Debug 日志。
字段语义、块的渲染规则，以及 Android 用的同一份快照契约，都在
`aqua_core/doc/diagnostics.md`（字段速查见其 §7）。本文件只记 CLI 特有的两件事：
**有哪些 source** 与 **两条节奏**。

## Source 清单（块名）

Server（`aqua_server_cli`，8 个块）：

```text
state  audio  capture  pktz  queue  dsp  net  sess
```

Client（`aqua_client_cli`，6 个块）：

```text
state  net  jb  jc  pb  stream
```

块内是 `k=v`；计数类字段写成 `T/D/R`（累计 / 本拍增量 / 每秒速率），由 `RateCounter`
产出——不是每个计数一条独立字段，这是它不撑爆一行的原因。注册点位见
`aqua_app/cli/client_main.cpp` / `server_main.cpp` 的 `line.add_source("模块名", ...)`。

`jc` 是 TargetController 的当期结算（target 为什么是这个值、被什么夹住），只有
自适应模式下有内容；`--jb-fixed-target` 关掉自适应后该组无意义。

## 两条节奏

- **1s 诊断行**：专用 diag 线程刷新一次快照并打一行（不占网络 ioc）。只在
  `--log-level debug` / `trace` 下输出；Debug 关闭时连 source 都不求值。
  Android 侧消费同一份快照，但速率由 App 自己差分，与本行无关。
- **500ms control poll**：不是音频控制器，只观察 runtime 是否进入 `Degraded` /
  播放链 Fatal 等终态并据此停止。**不能把它理解成"500ms 一次播放调整"**——
  JitterBuffer 完全由 playback callback 驱动。
