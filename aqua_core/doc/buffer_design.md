# JitterBuffer 算法设计（执行层）

本文是 JitterBuffer **执行层**的算法与边界设计文档（水位、episode、reanchor 的判定细节）。三层分工：

- **执行层（本文）**：`push()` / `pull()` 里的状态机 —— 收到什么、当前该 Fill 还是 Drop、何时 reanchor；
- **决策层**：谁观察网络、谁决定 `target`、为什么这样决定 —— `jitter_buffer_control_design.md`；
- **接口/接线/配置**：模块 API、C API/CLI 透传、Runtime 装配 —— `modules/jitter_buffer.md`。

数值一律见 `configuration_reference.md`；日志点位见 `modules/observability.md`。

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

阈值在构造期转换成 slot 数并四舍五入；N 至少 4，避免小容量量化后 warning 区塌缩。自适应模式下 target 是动态的，
水位带的缩放规则见下一节。

### 6.1 自适应模式下水位带随 target 缩放

上表的比例是 **固定模式**（`--jb-fixed-target`）的取值，同时也是自适应模式的缩放基准。自适应模式下
`TargetController` 每包改写 target，水位带由 **构造期预计算的带表**按当前 target 现算。乘数不是在运行期做除法，
而是把配置比例先整数化再比：

```text
multiplier_band   = round_pct(ratio_band, N) / round_pct(target_ratio, N)     # 构造期一次
band_slots(target) = lround(target × multiplier_band)                          # 查表，RT 安全
```

这样 `bands(起步 target)` 与固定模式配置逐值一致，target 缩放时保持同一组倍率。

- **必须整组缩放，不能只改 target**：`JitterBuffer::create` 的 config 校验强制
  `warning_low < normal_low < target < normal_high < warning_high`。只把 target 折小会低于 `normal_low` 而被拒绝 ——
  自适应模式会直接起不来。因此 `ClientRuntime::setup_playback` 在自适应路径上把整条带按同一 `scale` 折过。
- **为什么用查表而不是每个带各存一个原子**：旧实现"target 与四个带分五次独立 store、RT 侧分五次读"存在撕裂窗口， 诊断读到的
  `bands[]` 可能来自不同 target 的快照。查表让 `decide()` 与诊断只能读到同一 target 下的一组带值。 要同时看多个带请用
  `bands()`，不要连着读四个单值 getter。
- **target 的结构上限是 `2/3 × N`**（不是 N 本身）：`warning_high ≈ 1.5 × target`，target 顶到 N 时整条高水位带 落到 ring
  之外、DROP 失效；consumer 把 lead 顶满 ring 后新到的包全部 slot-busy 拒收 —— 人为造洞，随后在洞上 欠载（实测
  `busy ≈ 25% rx`、`underrun ≈ 30%`，UDP 零丢包而声音全破）。推导见
  `jitter_buffer_control_design.md` ADR-2。
- **固定模式逐值不变**：从不调用 `set_target_slots`（`--jb-fixed-target`）时，查表结果与改造前的固定阈值完全一致。
- **容量下限 4 是结构性的**：`N = 4` 时整除后 `warning_low == normal_low`（严格序退化为非严格），阶梯填充带消失，
  `decide()` 退化为更积极的 hold-fill —— 行为安全，但不是设计意图。

### 6.2 三时钟域：执行层与决策层的边界

本文档描述的 Fill / Drop / reanchor / concealment 全部属于 **播放时钟**，只由 RT callback 驱动。三个时钟域的职责
必须钉死，跨域只经原子快照交换，任一域不直接调用另一域的方法：

| 时钟域   | 驱动                    | 只许做什么                                                                               | 产出                                     |
|----------|-------------------------|------------------------------------------------------------------------------------------|------------------------------------------|
| 网络时钟 | push strand（UDP 收包） | estimator 统计、controller 算 target、**producer 侧 `push()`** 接受/拒绝与 reanchor 探测 | `target_slots_`、`reanchor_request_seq_` |
| 播放时钟 | RT callback             | 水位判定、Fill/Drop、reanchor 应用、concealment                                          | PCM、`underrun_events_`                  |
| 监督时钟 | 500ms `poll_control`    | 低频结构状态：设备切换、几何地板校正                                                     | `min_target_`                            |

**注意 `push()` 属于网络时钟，不是 RT**：reanchor 的"是否请求"在 producer 侧判定并只写一个 mailbox 原子 （"何时应用"由
consumer 在 `pull()` 中选择安全时机）——这是 reanchor 全链路不需要锁的原因。同理，
`used_slots_` / `highest_seq_` / `oldest_seq_` 都是两边各自 relaxed 读写的原子，语义上允许看到对方稍早的快照。

完整推导（含"controller 为什么事件驱动、不加 timer"）见 `jitter_buffer_control_design.md` §2。

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

启动水位为 startup_level（默认 50%，独立于稳态阈值序，可调 (0,1]）：锚定即通知音频 线程开始消费，为锚定后仍在涌入的帧留
headroom——若等到 target（60%），通知音频线程 的间隙里网络推入可把低容量 JB 打满（deadline-high Drop 抽搐）。默认 50% 又高于
normal_low（35%），提供足够的抗抖动垫层；锚定后 lead 位于 normal 区，稳态自然向 target 漂移。

并快照当前 slot 是否真的存在。建立锚点前还会二次读取 oldest/highest；如果两次快照变化，则本次 pull 继续输出静音，下一次再尝试，避免在
producer 并发更新时锚到已过期窗口。

## 8. Pull 的核心行为

### 8.1 Normal

直接按 `play_seq` 连续读取真实 slot，输出多少消费多少。一个 OS callback 可以跨越一个或多个 slot；`read_offset` 保存当前
slot 的消费位置。

### 8.2 缺帧

当前 `play_seq` 没有对应 Ready slot：输出 `F` 帧静音，但仍推进播放时间线。缺帧不会阻塞等待未来网络包。

**就绪快照在槽边界刷新**：消费循环在每个 slot 边界（`read_offset == 0`）重新快照当前 slot 的 ready 状态，而不是只依赖
`advance_slot()` 留下的缓存。这是必要的不变量——underrun 完全排空后
`play_seq > highest`，pull 走静音守卫路径直接返回、不更新快照；此后新帧恰好落在 `play_seq` 时， 若沿用残留的快照会把 READY
槽误判为缺帧静音并 `advance_slot()` 跳过。producer 与 pull 形成 1:1
步进时每帧都被同样跳过，播放头在静音中前行、真实数据全部丢失（"静默饿死"，回归测试：
`PullAfterFullDrainDoesNotSilenceFreshSlotAtPlayhead`）。部分槽读取期间 slot 不会被回收 （advance
才回收），故只需在槽边界刷新，部分槽内延续读取不必重复快照。

**Phase 2 concealment 路径**：若 `concealment.enabled && max_slots > 0` 且存在 `last_pcm_`， 缺帧 slot 不直接静音，而是把上一真实
slot 的 PCM 重复一次，并按 `(max_slots - i) / max_slots`
线性淡出（i = 已连续掩盖的 slot 数，0-indexed）。第 `max_slots + 1` 个起退回静音（强制上限，见 `jitter_buffer_control_design.md` ADR-8）。掩盖帧计入
`concealed_slots / underrun_frames` 但 **不**计入 `pull_silence_frames`，
便于在诊断中区分"在掩盖的欠载"与"真正静音的欠载"。

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

当 `s` 明显领先当前窗口时（`s - play >= N`）：

- 如果跳跃超过 `config::JB_MAX_REANCHOR_JUMP_FRAMES = 100000`（`buffer_config.h`），认为请求荒谬，拒绝该帧并增加 sanity
  rejection；

- 否则不立即改 playback timeline，只通过 atomic `reanchor_request_seq_` 发布“候选新锚点”；多个请求取最大的 sequence。
  触发帧继续走正常占用路径（测试锁定：reanchor 快路径依赖触发帧已在环中，`highest` 已指向远端）。

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

`advance_slot()` 的顺序也有意是： **先推进** **`play_seq`，再回收旧 slot**。这是为了防止 producer 在“slot 清空但 play_seq
尚未前移”的窗口内重新写入一帧旧 sequence。

## 10. 为什么 pull 完整输出

`pull(output)` 正常情况下填满整个 output：

```text
real PCM + missing silence + low-water hold silence
```

这样底层 backend 不会因为 callback 未填满而继续重复播放上一次缓冲中的残留数据。

只有 output 非法（空、不能整除 frame_bytes 等）时才返回 `frames_filled=0`。

## 11. 实时约束

`pull()` 禁止：

- mutex

- heap allocation

- system call

- blocking wait

- synchronous log（源码提供的 `AQUA_JB_RUNTIME_THREAD_DEBUG_LOG` 是开发期异常开关，开启会破坏 RT 契约）

## 12. 统计语义

| 计数器                               | 含义                                                                                                                                                                                                                                                                                       |
|--------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `used_slots`                         | 物理占用的 slot 数（真实 occupied，不是 sequence lead）                                                                                                                                                                                                                                    |
| `water_level`                        | `lead / N`，**始终在 [0,1]**：lead 与 water_level 在代码侧均已裁剪到容量（`jitter_buffer.cpp:236`、`:252`）。为什么裁剪：reanchor 请求待应用时 `highest` 会被暂时抬高、lead 可能瞬时超过 capacity，若不裁剪就会给出 >1 的"伪水位"误导诊断；裁到 [0,1] 让上层能直接按百分比解释缓冲充盈度。 |
| `push_accepted`                      | 接受的帧                                                                                                                                                                                                                                                                                   |
| `push_rejected`                      | 被拒总数（= 下面四类之和）                                                                                                                                                                                                                                                                 |
| `push_rejected_late`                 | 迟到（sequence 已越过播放位置）                                                                                                                                                                                                                                                            |
| `push_rejected_slot_busy`            | 目标槽非 Empty（重复帧或 producer 正在写入）                                                                                                                                                                                                                                               |
| `push_rejected_invalid`              | `frame_count` / 字节数不符，或 sequence 为哨兵值                                                                                                                                                                                                                                           |
| `push_rejected_sanity`               | 跳跃超过 `config::JB_MAX_REANCHOR_JUMP_FRAMES`（100000）                                                                                                                                                                                                                                   |
| `pull_silence_frames`                | 实际输出静音的帧数                                                                                                                                                                                                                                                                         |
| `fill_episodes`                      | 进入 Fill episode 的次数                                                                                                                                                                                                                                                                   |
| `fill_corrected_slots`               | Fill 期间因慢放重播而多播的 slot 数                                                                                                                                                                                                                                                        |
| `reanchor_count`                     | 应用 reanchor 的次数                                                                                                                                                                                                                                                                       |
| **`underrun_events`**                | 播放头推进到"无真实 PCM 可用"slot 的边沿次数（连续缺帧算 1 次）                                                                                                                                                                                                                            |
| **`underrun_frames`**                | 欠载总帧数（含被 concealment 掩盖的帧）                                                                                                                                                                                                                                                    |
| **`max_consecutive_underrun_slots`** | 本次运行最长单次欠载 slot 数（验收"单次≤3 包"用）                                                                                                                                                                                                                                          |
| **`concealed_slots`**                | 被 repeat-last 掩盖的 slot 数                                                                                                                                                                                                                                                              |
| **`concealed_saturated_slots`**      | 超过连续上限、退回静音的 slot 数                                                                                                                                                                                                                                                           |
| **`late_useful_packets`**            | 迟到但"本可用"的包（落后播放头 ≤ `max_slots`），用于评估 late/reorder 插入价值                                                                                                                                                                                                             |

注意：

- 不要用 `pull_silence_frames` 直接推断 UDP 丢包：静音可能来自网络缺帧，也可能来自低水位强制 Hold / 恢复阶段；warning 区
  的慢放重播不计入静音；concealment 路径的掩盖帧 **不**计入静音（计入 `underrun_frames`）。
- `push_rejected_late` 存在少量误报：consumer 已消费该槽、producer 的 CAS 失败时也会计入，但数据其实已经被播放。
- `underrun_ratio` 推荐在诊断层用 `underrun_frames / pull_frames` 计算（`pull_silence_frames`
  同样参与分母即可表达"静音 + 掩盖"两类的总欠载比）。

## 13. 与设备切换的关系

回放设备切换时 JitterBuffer **不被清空**：`PlaybackManager` 的 switch 事务 stop 旧流（join）后 start 新流，事务期间没有
消费者，网络侧继续 `push()`，水位自然上涨；新流接上后从原 `play_seq` 继续消费。切换间隙表现为一次普通的高水位波动，没有
reanchor、没有 pre-roll 重来。

服务端采集切换在 **对岸**表现为一次 packet gap：本侧 JB 走 §8.2 / §8.3 的缺帧静音与低水位 Fill 路径吸收。两种情况都不应导致
`reanchor_count` 增长——若增长，说明时间线被误重置。
