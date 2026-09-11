# Aqua JitterBuffer 自适应延迟改造（设计文档）

> 本文档是自适应 JitterBuffer 的**设计与规格来源**（原改造草案，2026-09-11 按
> 双机/三机实测定稿）。参数原理与调参方法见 §A（文末），模块级实现说明见
> `modules/jitter_buffer.md`。§18 是实施状态与达成度记录。

## 目标

在**不重写现有 JitterBuffer 架构**的前提下，把当前固定 target/watermark 的播放策略升级为：

> **根据真实网络到达抖动动态选择尽可能低、同时满足稳定性要求的 target latency。**

现有的以下机制全部保留：

* slot/ring buffer
* SPSC ownership
* startup/pre-roll
* sequence ordering
* late/duplicate handling
* Filling / Dropping
* deadline
* reanchor
* partial slot consumption
* endpoint switch 后的连续时间轴
* 现有 diagnostics 查询接口和计数器

本阶段**不做**：

* Opus
* FEC/PLC
* resampler
* clock-drift correction
* PacketBuffer/PlayoutController 大拆
* 动态 JB capacity
* RT 路径改造
* 新线程
* 第二套 playback buffer

---

# 1. 核心设计原则

### 1.1 JB 仍然是当前唯一 playback buffer

不要把现有 JB 拆成新的 PacketBuffer / PlayoutController 架构。

只允许增加两个职责明确的小组件：

```text
JitterBuffer
    ↑
TargetController
    ↑
JitterEstimator
```

其中：

```text
JitterEstimator
    = 只负责观察网络

TargetController
    = 只负责计算 target

JitterBuffer
    = 继续负责实际播放与 Fill/Drop/reanchor
```

Estimator 不直接操作 JB。

Controller 不直接执行播放。

---

# 2. JitterEstimator

创建纯粹的 `JitterEstimator`。

要求：

* 无 IO
* 无锁
* 无动态分配
* O(1) 每包处理
* 只运行在 UDP/network push side
* pull/RT playback side 不执行 estimator 更新

输入：

```text
sequence
timestamp
arrival_time
sample_rate
frame_count / packet duration
```

输出 observation：

```text
transit
interarrival_jitter
base_delay
arrival_interval
reordered
duplicate
```

## 2.1 Sequence

sequence 用于：

* 新旧 packet 判断
* duplicate 检测
* reorder 判断
* gap 统计
* extended sequence 计算

不要用 sequence 单独推断 network latency。

内部仍使用现有 `uint64_t` extended sequence。

---

## 2.2 Timestamp

RTP timestamp 用于：

> **描述 sender media timeline。**

不要因为 timestamp 出现就重写现有 JB timeline。

第一阶段它主要用于：

```text
sender media duration
vs
receiver arrival time
```

建立网络 arrival statistics。

不要把：

```text
timestamp jump
timestamp non-monotonic
```

直接变成 JB recovery action。

Timestamp discontinuity、network loss、reordering 必须继续分别处理。

---

## 2.3 Transit

使用相对 transit：

```text
transit(n) =
    (arrival(n) - arrival(0))
    -
    (timestamp(n) - timestamp(0)) / timestamp_rate
```

不依赖 timestamp 的随机初始 offset。

---

## 2.4 RFC 3550 Jitter

实现 RFC 3550 A.8 的 O(1) interarrival jitter estimator：

```text
J += (|D| - J) / 16
```

但是：

> **J 只是网络抖动观测量，不直接等于 target latency。**

不要把：

```text
target = k * J
```

冻结成最终算法。

### 2.4.1 stall 与抖动必须分离（2026-09-11 实测修订）

RFC 3550 的 D 按原始口径**含丢包/断流间隙**。实测证明这对 target 计算是错的：

> 到达间隔超过 **5 个包周期**（burst 串间间隔 ≈2.7 包周期，阈值留近一倍余量）
> 即判为 **stall（时间断流）**，**不进 J**，只单独计数（`stall_events`）。

原因：stall 不是"抖动"而是断流/严重拥塞。把它喂进 J 会让 J 瞬间飙升数倍、
target 过冲后按跌侧限速花十几秒才回落。双机实测：一次 170ms 的 Wi-Fi stall
把 target 从 7 顶到 21 挂了 14s，且 stall 的 45 个滞后包随后 burst 回补把
30 槽环形打爆（busy 拒绝 + 4 次 reanchor）。

stall 的代价由**欠载反馈（penalty）和 reanchor** 负责，不归 J 管。
这与 NetEq DelayManager 把长延迟/间隙交给 peak handling 而非 IAT 均值是同一思想。

---

## 2.5 Base delay

维护一个长期的最低 transit estimate：

```text
base_delay = robust minimum / low percentile of transit
```

它表示：

> 网络路径的长期最低延迟/底噪。

它**不是 clock drift compensation**。

禁止把慢性声卡 clock drift 解释成 base delay correction。

---

## 2.6 Reordering

必须区分：

```text
new packet
duplicate
reordered but potentially useful
late
too late
```

第一阶段可以继续保持现有：

```text
late → drop
```

但必须开始统计：

```text
reordered_packets
late_packets
too_late_packets
duplicate_packets
```

是否实际利用 late/reordered packet，留到有真实数据后再决定。

---

# 3. TargetController

创建独立 `TargetController`。

输入：

```text
base_delay
jitter estimate
jitter variation / percentile
burst information
current target
buffer capacity
current underrun history
```

输出：

```text
target_slots
```

## 3.1 第一版算法

第一版允许采用：

```text
target =
    clamp(
        base_delay
        + jitter_margin,
        min_target,
        max_target
    )
```

其中 `jitter_margin` 可以从：

```text
k × J
```

开始实验。

但实现必须允许未来切换到：

```text
percentile / histogram
peak / burst detector
hybrid strategy
```

不要把公式硬编码成系统架构。

WebRTC NetEq 的 DelayManager 就不是简单使用单一 J 值，而是维护 inter-arrival statistics、目标 buffer level、minimum/maximum delay 和 peak handling。可以参考这种思想，但不要直接复制 NetEq 的复杂状态机。

---

# 4. Target 必须具有控制迟滞

TargetController 至少需要：

```text
min_target
max_target
rise_rate
fall_rate
deadband
```

原则：

```text
网络突然恶化
    → target 可以较快上升

网络恢复稳定
    → target 必须缓慢下降

短暂 jitter spike
    → 不允许 target 来回抽动
```

避免：

```text
10
8
11
9
12
```

这种 buffer target oscillation。

### 4.1 现状（2026-09-11 实测）

* `deadband` 实际取值 **0**：对称死区与"跌侧不限死区 grind 到底"叠加会产生
  永久偏移（实测 target 卡在 desired−1）。阻尼由"涨快跌慢"（涨即时、跌 1 槽/秒）
  承担，本身不振荡。
* 仍存在**幅度 1 槽的微跳**（7↔8）：源于 J 随 burst 拍频在 4.5↔5.4ms 摆动
  × `ceil()` 取整。但此时 lead 稳在 normal 带 `[4,10]` 内，**不触发 Fill/Drop**，
  听感无感（fill_duty≈0.002）。判"真振荡"看 fill/drop duty，不看 target 方向
  反转次数（后者会误报）。
* `rise_rate` 未单列：上涨恒为即时（恶化必须立即跟进），只有 `fall_rate` 受限。

Roc 对 latency tuning 同样区分 responsive/gradual 等不同控制 profile，说明“如何追 target”本身就是独立于“target 是多少”的控制问题。

---

# 5. Target 与 Capacity 严格分离

第一阶段：

```text
capacity = 固定
target   = 动态
```

不要做 live-resize。

原因：

```text
capacity
    同时影响：
    - slot mapping
    - maximum lead
    - memory
    - sequence geometry
```

本阶段只优化：

> **在已有 capacity 中选择更合理的 target。**

---

# 6. Startup policy

不要因为 target 从：

```text
18 slots
```

下降到：

```text
3~5 slots
```

就直接：

```text
first packet → play
```

startup 必须拥有自己的下限。

建议：

```text
startup_target =
    max(min_startup,
        controller_target)
```

第一版实验范围：

```text
2~4 packets
```

最终数字由真实 underrun 数据决定。

目标：

> **尽可能快速启动，同时不产生稳定状态下无法接受的初始 underrun。**

不要预先冻结“必须 1 packet”。

---

# 7. 现有 Fill / Drop 机制继续保留

不要删除：

```text
Filling
Dropping
deadline
reanchor
```

Adaptive target 只改变：

```text
什么水位算“偏低/偏高”
```

而不是重新创建一套 correction state machine。

即：

```text
TargetController
        ↓
target

existing JB
        ↓
Fill / Drop / Reanchor
```

---

# 8. Underrun budget 是本阶段核心评价指标

不要把：

```text
“3 slots”
```

作为硬性成功标准。

真正的优化目标：

> **在指定 underrun budget 下，让 target 尽可能低。**

必须统计至少：

```text
played_frames
underrun_frames
underrun_events
max_consecutive_underrun
```

并观察：

```text
underrun_ratio =
    underrun_frames / played_frames
```

建议实验时同时观察：

```text
target p50
target p95
target p99
underrun ratio
reanchor rate
fill/drop duty
```

### 8.1 budget 的适用范围（2026-09-11 实测修订）

`underrun_ratio < 0.001` 与 `单次 ≤ 3 包` 只对**无损、无 stall 的链路**成立：

* **丢包**：1% 丢包天然产生 ≈1~2% underrun（丢的包是真缺帧，concealment 只
  掩盖 3 包以内）。budget 无法也不应在丢包链路上成立——验收时按链路条件分档。
* **stall（>5 包周期）**：任何系统都救不了（除非 buffer ≥ stall 时长），
  它不属于 jitter，不该用 target 去覆盖。budget 验收必须**剔除 stall 区间**
  后看稳态值（实测干净 LAN 稳态 0.0004 达标，stall 时刻除外）。

SonoBus 的产品策略也是明确在“最低 latency”和“减少 dropout”之间做权衡，而不是把某一个固定 buffer size 当成普遍最优解。

---

# 9. Concealment

当前阶段只做 PCM 能合理做到的 concealment：

```text
missing packet
    ↓
repeat last valid PCM
    ↓
短 fade
    ↓
silence
```

要求：

* concealment 必须有最大连续长度
* 超过上限后进入静音
* 不修改媒体 sequence/timestamp
* 不修改 target estimator 的统计语义

第一版不要为了“听起来更好”加入复杂 DSP。

---

# 10. Reanchor

**本阶段禁止删除 reanchor。**

自适应 target 与 reanchor 的职责不同：

```text
TargetController
    = 正常状态下应该保持多少 buffer

Reanchor
    = 时间轴进入异常状态后的恢复
```

只有在实际 telemetry 证明：

```text
adaptive target
    与
reanchor
```

存在系统性冲突后，下一阶段再决定是否简化。

禁止提前以“理论上重复”删除现有 recovery 机制。

---

# 11. Diagnostics

新增/保留以下观察值：

```text
jitter_ms
base_delay_ms
transit_ms
target_slots
target_ms
lead_slots
lead_ms

reordered_packets
late_packets
duplicate_packets
sequence_gap_events

underrun_events
underrun_ratio
max_consecutive_underrun

fill_duty
drop_duty
reanchor_count
```

特别要求：

```text
current target
+
actual lead
+
actual jitter
```

必须能在同一个 snapshot 中看到。

否则无法判断：

```text
target 为什么变化
```

以及：

```text
JB 为什么没有达到 target
```

---

# 12. Phase 0：Observation-only

Phase 0 不改变任何 playback behavior。

输入：

```text
(seq, timestamp, arrival_time)
```

输出：

```text
transit
jitter
base_delay
reorder
duplicate
gap
```

测试场景：

```text
1. clean LAN
2. ±1ms jitter
3. isolated jitter spike
4. burst loss: 5 packets
5. reorder: 2 packets
6. duplicate
7. steady +50ppm sender clock
8. timestamp wrap
9. sequence wrap
```

要求：

```text
JB playback decision
    与当前版本完全一致
```

---

# 13. Phase 1：Adaptive Target

接入：

```text
JitterEstimator
      ↓
TargetController
      ↓
existing JitterBuffer
```

固定 target 仍提供 diagnostics/debug override：

```text
adaptive = on/off
```

目标不是证明：

```text
target == 3 packets
```

而是证明：

```text
clean network
    → target 自动下降

jitter increase
    → target 上升

network recovery
    → target 缓慢下降

长期运行
    → target 不 oscillate
```

并且：

```text
underrun budget
```

满足要求。

---

# 14. Phase 2：PCM Concealment

加入：

```text
repeat-last
fade-out
silence ceiling
```

late packet 暂时继续：

```text
drop
```

但开始记录：

```text
late packet usefulness potential
```

只有 Phase 0/1 数据证明 late reorder 在 Aqua 的实际网络中有价值时，才实现插入。

### 14.1 结论：不实现 late 插入（2026-09-11 实测定案）

三份双机日志（有线 + Wi-Fi 双向）`late_useful_packets` **全为 0**：滞后包从不
落在 conceal 窗口内，即 Aqua 的实际网络上"迟到但本可用"的包不存在（要么准时、
要么迟到到 ring 外）。Phase 0/1 数据已给出答案：**late/reorder 插入不做**，
`late → drop` 维持为最终行为，统计口径保留。

---

# 15. Phase 3：简化必须数据驱动

在拿到长期 telemetry 后，再评估：

```text
reanchor
EpisodeDir
watermark rules
```

原则：

> **能证明冗余，再删除。**

不得因为“新 Controller 看起来已经覆盖这个职责”就提前删除经过 v1 实测的 recovery machinery。

### 15.1 现状：什么都不删（2026-09-11 实测定案）

* **reanchor 保留**：stall 时 45 个滞后包 burst 回补把 30 槽环形打爆，正是
  reanchor（4 次）把时间轴拽回正轨。它不是冗余，是 stall 恢复的主力。
* **Fill/Drop 保留**：stall 恢复期靠 Fill 重新蓄水、靠 Drop 甩掉回补 backlog。
  稳态占用率极低（fill_duty≈0.002、drop_duty≈0.01）但属于"平时不用、关键时刻
  救命"，不满足"已证冗余"的删除条件。
* **EpisodeDir / watermark rules 保留**：watermark 已随 target 等比缩放，是当前
  唯一的 Fill/Drop 触发源，无替代。

唯一值得改（不是删）的：排空/hold 路径的硬静音可在 concealment 开启时改走
repeat-last（听感优化，非架构变动）。

---

# 16. 外部参考项目

只借思想，不复制整体架构。

### WebRTC NetEq

重点参考：

```text
DelayManager
inter-arrival histogram
minimum/maximum delay
peak detection
target buffer level
late/reorder handling
```

不要引入整个 NetEq。

NetEq 的 DelayManager 本身就是一个专门的 adaptive target 层，并显式维护 packet IAT、target level、minimum/maximum delay 等状态。

### SonoBus

重点参考：

```text
manual latency control
automatic jitter buffer
lowest-latency vs dropout trade-off
```

它明确把 jitter buffer 当成可调的延迟/稳定性旋钮。

### Roc

重点参考：

```text
latency tuning
responsive vs gradual controller
queue-based latency measurement
长期 clock correction 与 latency tuning 的分离
```

不要复制 Roc 的完整 pipeline。Roc 本身就是面向实时网络音频，并同时处理 latency、loss recovery 和 clock-domain conversion。

### Snapcast

重点只参考：

```text
长期 clock synchronization
playback rate correction
```

本阶段不实现其 clock synchronization。Snapcast 的客户端持续与 server 做时间同步，并通过微调播放速率处理长期偏差。

---

# 17. 成功标准

这一阶段完成后，Aqua 应满足：

```text
① RTP packet 已作为现有 JB 的新输入格式
② JB 核心 slot/SPSC/episode 结构仍然存在
③ clean LAN 下 target 明显低于当前固定 60%
④ jitter 增大时 target 能自动增加
⑤ 网络恢复后 target 能平滑下降
⑥ target 不产生明显 oscillation
⑦ underrun ratio 在目标 budget 内
⑧ reanchor 仍然可靠
⑨ endpoint switch 不破坏 sequence/timeline
⑩ diagnostics 可以解释 target 为什么变化
```

最重要的评价指标：

> **在稳定性约束下，把 Aqua 的平均播放延迟压到尽可能低，而不是追求某个预设的固定 slot 数。**

---

# 18. 实施状态与达成度（2026-09-11 三机实测）

## 18.1 §17 成功标准逐条核对

| # | 标准 | 状态 | 证据 |
|---|---|---|---|
| ① | RTP 包作为 JB 输入 | ✅ | Phase 0 起 |
| ② | slot/SPSC/episode 结构保留 | ✅ | JB 状态机零改 |
| ③ | clean LAN target 明显低于固定 60% | ✅ | 7 slots(26ms) vs 18 slots(67.5ms) |
| ④ | jitter 增大 target 自动增加 | ✅ | stall 时 7→16→21（后被 2.4.1 抑制） |
| ⑤ | 恢复后 target 平滑下降 | ✅ | 1 槽/秒限速，日志可见 21→…→7 |
| ⑥ | target 不明显振荡 | ✅ | 仅 1 槽微跳，lead 在 normal 带内，fill/drop≈0 |
| ⑦ | underrun ratio 在 budget 内 | ✅(稳态) | 干净 LAN 稳态 0.0004；stall 区间除外（§8.1） |
| ⑧ | reanchor 可靠 | ✅ | stall 时 4 次 reanchor 恢复时间轴 |
| ⑨ | endpoint switch 不破坏 timeline | ✅ | 既有机制保留 |
| ⑩ | diagnostics 可解释 target 变化 | ✅ | target 变化日志带 jit/base/penalty/bands |

## 18.2 实测三场景（默认 gain=5 / penalty=1）

| 场景 | target p50/p95 | underrun_ratio | 备注 |
|---|---|---|---|
| 有线（laptop 做 server） | 7 / 7 | **0.0004 ✅** | 无振荡、无 reanchor，完美 |
| Wi-Fi（laptop 做 server） | 8 / 11 | 0.0010 | 小 stall 曾顶到 12，§2.4.1 已修 |
| Wi-Fi（laptop 做 client） | 7 / 18 | 0.0065 | 一次 170ms stall 顶到 21 + 4 reanchor，§2.4.1 已修 |

稳态全部正常；所有超标都来自 **stall 污染 J**（§2.4.1 已修）。

## 18.3 与"用户可调参数 JB"的差距

模型层已完成。差的是**产品层的用户入口**：

1. 目前只有 CLI 旋钮（`--jb-jitter-gain` / `--jb-min-target` / `--jb-capacity`）。
2. 要让 App 用户调，需把**一个简化旋钮**（低延迟↔稳定，映射到 k≈3..7 或三档）
   加进 C API 配置（ABI 末尾追加），普通用户不该看到 k。
3. 这是产品决策，不是 JB 技术债。

## 18.4 下一步优先级

1. ~~stall 污染 J~~ ✅ 本轮已修（§2.4.1）
2. App 端单旋钮（C API 追加，见 18.3）
3. hold/排空路径硬静音 → concealment 开启时走 repeat-last（听感优化）
4. （远期）server 发包整形：把 burst 摊平成匀速，target 可自动落到地板 4 slots

---

# 附录 A：实现思路与参数原理（详细版）

## A.1 三层结构与数据流

```text
UDP 收包 (push strand)
  └─ JitterEstimator.observe(seq, ts, ssrc, arrival_ns)
        → J(RFC 3550 均值) / transit / base_delay / stall_events / stall_peak(衰减峰值)
  └─ TargetController.update(base, J, t, jb->underrun_events(), stall_peak)
        desired = clamp((base + margin) / packet_ms,
                        min_target(=max(--jb-min-target, 几何地板+1)) + 欠载惩罚,
                        2/3 × capacity（结构上限，见 A.2）)
        margin = max(k×J, stall_peak/packet_ms + 1包)
        涨即时 / 跌 fall_rate 限速 / 涨后 dwell 锁跌 / 惩罚按事件累加衰减
  └─ JitterBuffer.set_target_slots(target)
        → 四档水位带按构造比例重算  wl=0.25T  nl=0.50T  nh=1.25T  wh=1.50T（起步 4 槽整数化所得）
  └─ pull() (RT 线程) 只读这些带：lead<nl→Fill，lead>nh→Drop，排空→conceal
```

- **JitterEstimator = 只观察网络**（无 IO/无锁/无分配/O(1)），不驱动 JB。
- **TargetController = 只算 target**（无 IO/无锁/无分配/O(1)），不执行播放。
- **JitterBuffer = 唯一播放缓冲**，继续负责 Fill/Drop/reanchor/concealment。

所有 `jb-*` 参数都只作用在中间两环（算 target、换算水位带），不动状态机。

## A.2 控制律详解

### target 的四个决定因素

```text
desired = ceil( clamp( base_slots + margin_slots,
                       effective_min,
                       2/3 × capacity ) )
margin_slots = max( k×J/packet_ms, stall_peak/packet_ms + 1 )
```

| 项 | 来源 | 作用 |
|---|---|---|
| `base_slots` | `base_delay_ms`（transit 累积最小值，>0 才用） | 路径底噪，基本不用（burst 下为负被夹 0） |
| `k×J/packet_ms` | JitterEstimator 的 J × `--jb-jitter-gain` | **主力预测项**：按平均抖动预留余量 |
| `stall_peak/packet_ms + 1` | JitterEstimator 的 stall 峰值（近期最坏到达间隙的衰减最大值，10ms/s 回落） | **尾部补丁**：被 stall 门剔除出 J 的拥塞间隙由这项接管；与 k×J 取 max 不重复计 |
| `effective_min` | `max(--jb-min-target, 几何地板+1) + 欠载惩罚` | **下限**：几何地板（无条件托底）+ 闭环安全网 |
| `2/3 × capacity` | `--jb-capacity` | **结构上限**：target 最多用下 2/3，上 1/3 留给抖动吸收 |

> **几何地板的口径**：一次 playback callback 消耗 `ceil(callback_frames / F)`
> 个包。构造期用 start 时的请求帧数（`frames_per_buffer`）估算；playback 启动后
> `ClientRuntime` 以 `pull_playback` 观测的**实际** callback 帧数为准（RT 线程
> 原子缓存、控制线程 500ms 轮询比对，变化即
> `TargetController::update_geometric_floor()` 校正）——backend 自选周期
> （`frames_per_buffer=0`）或设备切换事务改变 callback 几何时，地板与下限自动
> 跟进，不需重启会话。

> **为什么上限是 2/3 而不是 capacity 本身**：水位带随 target 等比放大
> （warning_high≈1.5×target）。target 顶到 capacity 时整条高水位带落到 ring
> 之外、DROP 机制失效；consumer 把 lead 顶满 ring 后，新到的包全部撞上未消费
> 槽位被 slot-busy 拒收——人为造洞，consumer 再在洞上欠载（极端 gain 实测：
> busy≈25% rx、underrun≈30%，UDP 零丢包但声音全破）。上 1/3 与固定模式的
> 0.6 target / 0.9 ceiling 是同一结构。

### 为什么 J 是均值而 target 要覆盖峰值 → k 的来源

RFC 3550 的 J 是"到达间隔偏差绝对值的滑动均值"。Aqua 的 server 以 capture 周期
成串发包（480 帧/10ms 抓一次、180 帧/包 → 每 10ms 一串 2~3 包，串内间隔≈0），
这种确定性 burst 下 J≈4.6ms 而 transit 峰峰值 8.75ms ≈ 均值 2 倍；再叠加播放
callback 周期（512 帧 = 10.667ms）与发包周期（10ms）的拍频（160ms 扫过全部
相位），k=2 给的 3 slots 在最坏相位必然排空。k=5 才覆盖峰值（`tools/sim_jb_target.py`
扫 16 个相位验证）。

### 迟滞（防振荡）

| 机制 | 参数 | 行为 |
|---|---|---|
| 涨 | 无（恒即时） | 恶化立即跟进，不允许延迟 |
| 跌 | `JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC`（默认 1.0 槽/秒，已内部化） | 恢复缓慢，防 target 追瞬时下探 |
| 涨后锁跌 | `JB_ADAPTIVE_RISE_DWELL_MS`（默认 3000ms，已内部化） | 涨后窗口内不许跌 = 峰值保持，防 J 摆动致 target 来回跳 |
| 死区 | 固定 0 | 对称死区会把干净网钉在 desired−1（实测），故弃用 |

## A.3 参数速查与调参

### 用户级（CLI 暴露，有明确"延迟↔稳定"权衡）

| CLI | 默认 | 原理 | 何时调 |
|---|---|---|---|
| `--jb-jitter-gain` | 5.0 | 主力预测项：每 +1 ≈ 多 `J/packet_ms` 槽（本几何 ≈1.2 槽 ≈4.5ms）。**调到很大也不会失控**——target 被 `2/3 × capacity` 结构上限接住 | 干净网欠载超标→调大；延迟富余→调小 |
| `--jb-min-target` | 3 | target 硬下限。**有效下限 = max(本值, 几何地板 + 1)**——几何地板无条件托底，本旋钮只能抬高 | 地板之上仍欠载→调大；想压到地板以下做不到（需改 `JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS` 重编译） |
| `--jb-capacity` | 30 | **capacity N**：环形槽数 + 内存。自适应 target 上限 = `2/3 × N`（上 1/3 留给抖动吸收） | 抖动大→调大；要低延迟→调小（≥4，低于 4 五个水位带无法严格排序） |

### 开关（非旋钮）

| CLI | 作用 |
|---|---|
| `--jb-fixed-target` | 关自适应，回固定 `target=0.60N / startup=0.50N`（§13 要求的 A/B 对照） |
| `--jb-no-conceal` | 缺帧直接静音，不走 repeat-last + 淡出 |

### 已内部化（无 CLI 入口，改常量重编译）

这批参数只有一个很窄的合理区间，暴露出去只会制造误调；默认值与取值理由全部
集中在 `aqua_core/include/aqua/audio/buffer/buffer_config.h`（namespace
`aqua::config`，前缀 `JB_`）。

| 常量 | 默认 | 原理 |
|---|---|---|
| `JB_ADAPTIVE_DEFAULT_INITIAL_TARGET_SLOTS` | 4 | 起步 target（J 在约 16 包内收敛，随即被自适应拉走） |
| `JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC` | 1.0 | 恢复回落限速（槽/秒）。阻尼常数，只锁跌 |
| `JB_ADAPTIVE_RISE_DWELL_MS` | 3000 | 涨后锁跌窗口（ms）。Wi-Fi 省电发包致 J 摆动时防 target 微跳 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS` | 1.0 | 闭环安全网：每次欠载把下限顶高该槽数 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS` | 6 | 反馈抬升累计上限（槽），护栏 |
| `JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC` | 0.5 | 无新欠载时惩罚回落速率（槽/秒） |
| `JB_ADAPTIVE_DEADBAND_SLOTS` | 0 | 死区。实测会把 target 钉在 desired−1，故固定 0 |
| `JB_CONCEALMENT_DEFAULT_MAX_SLOTS` | 3 | 连续掩盖上限（包），超出转静音；0 = 关 concealment |
| `JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS` | 5.0 | 到达间隔超这么多包周期判为 stall（不进 J）；0 = 关检测 |

### 调参决策流程

1. **先默认跑**（gain=5），看 `target p50/p95`、`underrun_ratio`、`drop_duty`、听感。
2. **干净有线欠载 > 0.1%** → `--jb-jitter-gain 6`（+≈4.5ms）。
3. **抖动/丢包下偶发欠载** → 调大 gain。要改安全网强度就改
   `JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS` 重编译（已无 CLI 入口）。
4. **target 来回跳（方向反转多）且伴 drop_duty 升高** → 先确认不是 stall（看
   `network stall` 日志），再考虑 gain 调大让 target 稳定跨过 ceil 边界，或改
   `JB_ADAPTIVE_RISE_DWELL_MS`。
5. **想压延迟且欠载余量很大** → gain 减 1。target 已经贴着几何地板时再压
   没有意义：地板是结构性下限（见 A.2），CLI 给不出更低的值——要么改
   `JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS` 重编译（不推荐），要么接受它。
6. **stall 日志刷屏** → 说明链路本身断流频繁，不是 target 能救的；必要时改
   `JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS`。

## A.4 判读日志

```text
# target 为什么变（push strand，每次变化一条）：
ClientRuntime adaptive target: 7 -> 8 slots (30.0ms) jit_ms=5.34 base_ms=-10.19 \
    transit_ms=13.30 underrun_penalty=0.00 bands[wl=2 nl=4 nh=10 wh=12] packet_ms=3.750

# 刚才是断流不是抖动（stall 不进 J）：
ClientRuntime network stall: gap=170.2ms (3.750ms/包) stalls=1 jit_ms=4.61 \
    transit_ms=107.9 — 不进 J，由欠载反馈/reanchor 负责
```

- `underrun_penalty>0` 出现说明闭环在工作（预期，不是故障）。
- 判"真振荡"看 `fill_duty`/`drop_duty`，不看 target 方向反转次数（后者会误报）。
- 预算 `underrun_ratio<0.1%` 只对**无损无 stall** 的稳态成立（§8.1）。


