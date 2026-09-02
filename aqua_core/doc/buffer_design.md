# JitterBuffer 设计（当前冻结实现）

## 1. 目标

JitterBuffer 是 Client playback path 上唯一的应用层缓冲。它同时承担：

1. UDP 乱序重排；
2. 丢帧检测；
3. pre-roll；
4. 低水位时通过暂缓播放建立积压；
5. 高水位时跳过未来 slot 降低积压；
6. 缺帧时产生静音；
7. 时间线明显跳变时安全 reanchor。

它不是一个“根据毫秒睡眠”的 buffer。容量和调整动作的基本单位是 slot。

## 2. 内部几何

设：

- `N` = `capacity_slots`

- `F` = `frame_count`

- `B` = `format.frame_bytes()`

- `S = F × B` = 一个 slot 的 PCM 字节数

- `C = N × S` = storage 字节容量

环形存储只有 `N` 个 slot，每个 slot 预分配 `S` 字节。构造完成以后 `push()` / `pull()` 不再分配。

## 3. 序号与槽位

slot index：

```text
index = sequence % N
```

slot 头包含：

```text
state: Empty / Writing / Ready   (atomic)
sequence: uint64_t               (普通字段)
```

producer 只有在 CAS 抢到 Empty 后才写 sequence/payload，最后 release-store Ready；consumer acquire-load Ready
后才能读取。sequence 与 payload 的可见性由 state 的 release/acquire 建立。

## 4. SPSC 角色

严格角色：

```text
UDP/network thread ── push() ──► producer
AAudio/WASAPI playback RT ── pull() ──► consumer
```

`push()` 和 `pull()` 不使用互斥锁。JitterBuffer 的所有消费侧 episode 状态只由 consumer thread 私有持有。

## 5. 水位定义

已启动：

```text
lead = highest_seq - play_seq + 1, if play_seq <= highest_seq
```

未启动：

```text
lead = highest_seq - oldest_seq + 1
```

```text
water = lead / N
```

注意：`used_slots` 是真实 occupied slot 数；`water_level` 是 sequence lead。二者可以不同，不能用一个代替另一个。

## 6. 默认阈值

默认：

```text
warning_low  = 20%
normal_low   = 35%
target        = 60%
normal_high  = 80%
warning_high = 90%
```

因此状态意义为：

```text
<20%        低水位 deadline / 强 Fill
20..35%     warning Fill
35..80%     normal
80..90%     warning Drop
>90%        deadline-high / 强 Drop
```

阈值在构造期转换成 slot 数并四舍五入；N 至少 4，避免小容量量化后 warning 区塌缩。

## 7. 启动 pre-roll

初始没有 `play_seq`。收到数据后，只积累，不按“第一包到达”立即播放。

当：

```text
lead >= startup_slots
```

才建立：

```text
play_seq = oldest_seq
read_offset = 0
```

启动水位为 startup\_level（默认 50%，独立于稳态阈值序，可调 (0,1]）：锚定即通知音频
线程开始消费，为锚定后仍在涌入的帧留 headroom——若等到 target（60%），通知音频线程
的间隙里网络推入可把低容量 JB 打满（deadline-high Drop 抽搐）。默认 50% 又高于
normal\_low（35%），提供足够的抗抖动垫层；锚定后 lead 位于 normal 区，稳态自然向
target 漂移。

并快照当前 slot 是否真的存在。建立锚点前还会二次读取 oldest/highest；如果两次快照变化，则本次 pull 继续输出静音，下一次再尝试，避免在
producer 并发更新时锚到已过期窗口。

## 8. Pull 的核心行为

### 8.1 Normal

直接按 `play_seq` 连续读取真实 slot，输出多少消费多少。一个 OS callback 可以跨越一个或多个 slot；`read_offset` 保存当前
slot 的消费位置。

### 8.2 缺帧

当前 `play_seq` 没有对应 Ready slot：输出 `F` 帧静音，但仍推进播放时间线。缺帧不会阻塞等待未来网络包。

### 8.3 Fill

低水位进入 Fill episode。Fill **不是向网络缓冲中写入更多数据，也不是在 warning 区输出静音**，而是减慢 playback 时间轴：

```text
warning 区：重复当前 READY slot（慢放校正）
step=1：当前 READY slot 额外播放一次
step=2：当前 READY slot 额外播放两次
...
```

因此 Fill 不直接修改 `water`。`water = highest_seq - play_seq + 1` 仍只由时间轴状态自然计算；Fill 的效果是让 `play_seq` 相对
wall-clock 少推进若干 slot，使后续网络到达的 frame 有更多时间进入 JB。

一个 `AudioFrame` 的 `frame_count` 只是 slot 的协议大小，不再被当成 Fill 的“静音时长”。一个 OS playback callback 可以跨越多个
slot，也可以只覆盖一个 slot 的一部分；Fill 的 slot 重播状态由 JB 的 `read_offset` 与预分配状态维护，因此不依赖 callback 大小。

`< warning_low` 仍使用 `hold_until_target`：这是低水位/恢复阶段的强兜底，持续输出静音并停住 `play_seq`，直到重新达到
target。

### 8.4 Drop

高水位进入 Drop episode。Drop 不丢“字节”，而是跳过完整 slot：

```text
play_seq += skipped_slots
```

然后正常输出。这样可以快速把 sequence lead 拉回 target。

warning 区 step 使用 `WarningStepFn`；默认以 4 次连续 warning evaluation 为一个 growth interval，从 1 slot 起逐步放大，并受
`max_step` 限制。`max_step=0` 时自动取 `max(2, round(0.1N))`。

> deadline-high 是特殊情况：直接计算 `lead-target`，一次跳到 target 附近，不走温和增长曲线。

## 9. reanchor

### 9.1 为什么需要

如果 playback 已经运行，而网络突然出现大段 sequence 空洞/跳跃，仅靠每次 Drop 一个 slot 会产生 O (gap/N)
的人工追赶，且会让时间线长时间处于错误位置。

### 9.2 producer 行为

当 `s` 明显领先当前窗口时：

- 如果跳跃超过 `JITTER_BUFFER_MAX_REANCHOR_JUMP_FRAMES = 100000`，认为请求荒谬，拒绝该帧并增加 sanity rejection；

- 否则不立即改 playback timeline，只通过 atomic `reanchor_request_seq_` 发布“候选新锚点”；多个请求取最大的 sequence。

### 9.3 consumer 行为

pull 时先把 request 取入 consumer 私有的 `deferred_reanchor_seq_`。

满足以下条件之一时应用：

1. 当前时间线已经耗尽（`play >= highest`）；
2. 当前 lead 已经覆盖整个 capacity；
3. 未启动阶段已有候选锚点；
4. reanchor 长时间处于 Hold 且 lead 无进展，连续 5 次 pull 后强制应用。

应用时：

- 扫描 N 个 slot；只保留 `[sequence, sequence+N)` 窗口内的 Ready slot；

- 删除窗口外的 Ready slot；

- `play_seq = sequence`；

- `read_offset = 0`；

- 当前 slot ready 状态重新快照；

- 开启一次 Fill episode，要求重新积累到 target。

### 9.4 重要边界

reanchor 的清理只删除 Ready。Writing 不强行处理，因为 producer 仍可能正在持有该槽；后续 producer late recheck 会根据新的
`play_seq` 把已过时写入回收。

`advance_slot()` 的顺序也有意是： **先推进** **`play_seq`，再回收旧 slot**。这是为了防止 producer 在“slot 清空但 play\_seq
尚未前移”的窗口内重新写入一帧旧 sequence。

## 10. 为什么 pull 完整输出

`pull(output)` 正常情况下填满整个 output：

```text
real PCM + missing silence + low-water hold silence
```

这样底层 backend 不会因为 callback 未填满而继续重复播放上一次缓冲中的残留数据。

只有 output 非法（空、不能整除 frame\_bytes 等）时才返回 `frames_filled=0`。

## 11. 实时约束

`pull()` 禁止：

- mutex

- heap allocation

- system call

- blocking wait

- synchronous log（源码提供的 `AQUA_JITTER_BUFFER_RT_DEBUG_LOG` 是开发期异常开关，开启会破坏 RT 契约）

## 12. 统计语义

`used_slots` 是物理占用；`push_*` 是网络输入接受/拒绝原因；`fill_*` 是控制 episode/慢放校正；`pull_silence_frames`
是真正输出静音的帧数；`reanchor_*` 是时间线纠偏。

不要用“silence\_frames”直接推断 UDP loss：静音可能来自网络缺帧，也可能来自低水位强制 Hold / 恢复阶段；warning 区的慢放重播本身不计入静音。
