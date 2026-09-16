# JitterBuffer 决策层设计（JitterEstimator + TargetController）

本文描述 **决策层**：谁观察网络、谁决定 target、为什么这样决定。执行层（水位带 / FILL / DROP / reanchor / concealment
的状态机与边界）见 `buffer_design.md`；模块 API 与接线见 `modules/jitter_buffer.md`；数值一律引用
`configuration_reference.md`，本文不复述常量值。

改造过程的历史叙述（草案、Phase 划分、实施状态记录）不在这里——它在 git 历史里，或者已经转化成第 11 节的 ADR。

## 1. 目标与范围

**目标**：在固定容量 JitterBuffer 之上，让播放延迟自己找位置——网络干净时贴近结构性下限，链路恶化时按实测抖动 **有界地**
抬高，链路恢复后 **缓慢**回落。三个可验收的诉求：

1. 干净链路上不为了保险付延迟：`J → 0` 时 target 落到下限，不是停在起始值；
2. 抖动/拥塞下不欠载：`k×J` 覆盖抖动均值，stall 峰值项覆盖被 stall 门剔除的尾部；
3. 任何输入都不失控：target 有结构上限、有硬下限、单次事故不买入长期延迟债务。

**明确不做什么**（避免重复讨论）：

- **不改执行层算法**：Fill/Drop 曲线、reanchor 判定、concealment 规则全部保留，决策层只提供 `target_slots`；
- **不做音高修正 / 波形拼接**：concealment 上限是"整包重复 + 线性淡出"，理由见 ADR-8；
- **不引入新的缓冲层**：JB 仍然是 playback 路径上唯一的缓冲，决策层不持有 PCM；
- **不做 late 插入**：late 包只记 `late_useful_packets` 潜力，不改变播放时间线（ADR-8）；
- **base_delay 不进控制律**：只作为诊断量保留（ADR-1）；
- **不给 controller 加 timer**：它是事件驱动的（第 3 节推论 1）；
- **不暴露结构性约束**：2/3 上限、死区恒 0 这类不是旋钮，见 `configuration_reference.md` 的"结构性约束"小节。

## 2. 术语与几何

| 术语               | 定义                                                                                                                                          |
|--------------------|-----------------------------------------------------------------------------------------------------------------------------------------------|
| `F`（frame_count） | 一个包携带的 sample frame 数。由 server 在 Connect 时下发（默认档 175）。                                                                     |
| `packet_ms`        | 一个包的媒体时长 = `F × 1000 / sample_rate`（默认档 F=175、48kHz：175×1000/48000 = 3.646ms；F 随格式变化）。决策层所有槽/毫秒换算的唯一基准。 |
| `slot`             | 环形缓冲的一格，存一个完整 `AudioFrame`（= 一个 UDP 音频包）。                                                                                |
| `capacity`（N）    | 环形槽数 = `--jb-capacity`（默认 30 槽 ≈ 109ms）。                                                                                            |
| `target`           | **JB 该持有多少已到达的数据**（槽）。稳态中心，也是恢复目标。                                                                                 |
| `lead`             | `highest_received_seq - play_seq + 1`（槽）。当前实际持有量。健康态 `lead ≈ target`。                                                         |
| 水位带             | 由 `target` 按构造期比例推出的四档阈值（`warning_low / normal_low / normal_high / warning_high`），`pull()` 用它判断该 Fill 还是 Drop。       |
| 几何地板           | 一次 playback callback 消耗的包数 = `ceil(callback_frames / F)`。`target ≤ 地板` 意味着"每个 callback 必然把 JB 抽空"，是结构性不可能。       |

`target` 的物理意义是 buffer budget， **不是**网络单程延迟：它回答"要存多少包才能平滑播放"，不回答"包在路上飞了多久"。

### 三个时钟域（设计不变式）

控制环横跨三个时钟，职责必须钉死。跨域只经原子快照交换，任一域不直接调用另一域的方法。

| 时钟域       | 驱动                    | 只许做什么                                                            | 产出                            |
|--------------|-------------------------|-----------------------------------------------------------------------|---------------------------------|
| **网络时钟** | push strand（UDP 收包） | "我观察到什么"：estimator 统计、controller 算 target、JB 接受的包写环 | `target_slots_`（原子）         |
| **播放时钟** | RT callback             | "现在该播什么"：水位判定、Fill/Drop/reanchor/concealment              | PCM、`underrun_events_`（原子） |
| **监督时钟** | 500ms `poll_control`    | 低频结构状态：设备切换、几何地板校正                                  | `min_target_`（原子）           |

推论两条：

1. **controller 是事件驱动的（按包到达率采样），不加 timer 线程。** 真断流（0 包）期间没有可控对象——target 是为 **到达的数据**
   定水位，断流饥饿归播放时钟侧的 underrun / concealment / reanchor 处理。断流期间 controller 的 内部状态（`fall_carry` /
   dwell / penalty / 峰值衰减） **冻结是语义正确的**：冻结值是对网络的最后一次可用估计， 盲飞期间按墙钟继续衰减反而丢信息。恢复后第一个包立即刷新（stall
   峰值取到完整断流间隙，经 cap 后贡献有限）。
2. **RT 线程不写 controller。** 几何地板校正走 `last_callback_frames_` 原子缓存 + 监督线程比对 （`sync_geometric_floor`
   ），RT 侧只做一次 relaxed store。

## 3. 总体架构

```text
UDP 收包 (push strand)
  ├─ JitterEstimator::observe(seq, ts, ssrc, arrival_ns)
  │     → J(RFC 3550 均值) / transit / base_delay(仅诊断) / stall_events / stall_peak(衰减峰值)
  │     → tail 直方图 P99（|到达间隔−发送间隔| 的 2048 包滑动窗口；默认策略下它就是抖动项，
  │        legacy 的 k×J 才是影子；冷启动 128 包内发布 -1）
  ├─ TargetController::update(J, arrival_ns, underrun_events, stall_peak, tail_p99, stall_events)
  │     desired = ceil( clamp( margin_slots, effective_min, 2/3 × capacity ) )
  │     margin_slots = max( 抖动项, min(stall_peak/packet_ms + 1, cap) )
  │       抖动项默认 = P99/包周期 + 1（TailQuantile 策略；冷启动/回退走 k×J）
  │     涨即时 / 风暴冻结跌 / 涨后 dwell 锁跌 / 跌 fall_rate 限速（远距加速）/ 欠载惩罚抬下限
  │     影子 desired 同路径用 k×J 另算一份（`leg=` 列，只看不碰）
  ├─ JitterBuffer::set_target_slots(target)      ← 只写一个原子
  └─ JitterBuffer::push(frame)                   ← 执行层接受/拒绝（含 reanchor 探测）
pull() (RT 线程，只读)
  → 按 target 查表得四个水位带 → lead 低于 / 高于带 → Fill / Drop；排空 → concealment
  → 7 个拼接点 crossfade（DROP 着陆 / FILL 重播 / 掩盖进出 / 进出静音 / reanchor 后静音）
```

职责边界（不可越界）：

- **JitterEstimator = 只观察网络**：不驱动 JB，不触发任何 recovery；
- **TargetController = 只算 target**：不执行播放，不碰 `lead`；
- **JitterBuffer = 唯一播放缓冲**：执行层状态机全部留在 `pull()` / `push()` 内。

所有 `--jb-*` 参数只作用在中间两环（算 target、换算水位带），不动执行层状态机。

线程模型见 `../threading_and_lifecycle.md`；`target` 的写入是"push strand 单写 + RT/诊断多读"的 relaxed 原子，
水位带由构造期预计算的带表按 target 现算，不存在"target 已更新、band 还是旧值"的中间态。

## 4. 观测层（JitterEstimator）

### 4.1 RFC 3550 J 与它的不足

`J += (|D| - J) / 16`，`D = 到达间隔 - 发送间隔`。J 是 **均值型**观测量，而 target 必须覆盖 **峰值**：

> **2026-09 更新（发送端 pacing 之后）**：下面两条"burst 撑大 J"的证据是 **历史样本**——
> 当时 server 收到 capture 通知就一次性清空队列（480 帧/10ms 抓一次、180 帧/包 → 每 10ms 一串
> 2~3 包，串内间隔 ≈ 0），`J ≈ 4.6ms`、transit 峰峰值 `8.75ms ≈ 2×均值`。现在 server 按
> packet 周期 pacing 摊平发包（`modules/server_audio_path.md`），稳态 `J` 降到 **< 1ms**，
> `k×J` 不再是 target 的主要贡献项—— **margin 现在主要由 stall 峰值项决定**（§4.2/§4.3）。
> `k=5` 保持不变的理由：J 小时 k 的影响本就有限，而一旦链路出现整形/聚合（把到达重新变成
> 成串），"均值撑不起峰值"的问题会原样回来，k 仍需覆盖那个 ~2 倍的峰均比。

- 历史样本：确定性 burst 下 `J ≈ 4.6ms`，transit 峰峰值 `8.75ms ≈ 2×均值`；
- 再叠加 playback callback 周期与发包周期的拍频（最坏相位周期性排空），`k=2` 给出的 3 槽必然漏欠载。

k 的取值与推导见 ADR / `configuration_reference.md`（`JB_ADAPTIVE_DEFAULT_JITTER_GAIN`）。

### 4.2 stall 门：把断流从 J 里摘出去

到达间隔超过 `stall_threshold` 个 **包周期**即判为 stall（时间断流），该间隔 **不计入 J**，只记 `stall_events` 并刷新峰值。

为什么必须剔除：J 是均值，一次 100ms 瞬断会把 J 顶高好几毫秒，之后十几秒的 target 都被这次历史事故绑架（双机实测： 一次 170ms
的 Wi-Fi stall 把 target 从 7 顶到 21 并挂了 14s，随后 45 个滞后包 burst 回补把 30 槽环打爆 —— busy 拒绝

+ 4 次 reanchor）。

阈值必须显著大于 burst 串间间隔（≈2.7 包周期），否则正常发包会被误判；实测取 5 包周期，35ms 级及以上的真 stall （双机实测 35~
170ms）稳稳落在线外。`stall_threshold ≤ 0` = 关检测，回到裸 RFC 3550（ADR-6）。

**注意**：stall 只影响 J 与峰值，不影响 transit / base_delay —— 它们回答"这包多晚"的绝对量，不参与抖动统计。

### 4.3 stall 峰值跟踪与衰减

被 stall 门剔除的到达间隙是"**buffer 必须独自挺过的最长时间**"，近期最坏值直接指示 target 需要的水位 （NetEq DelayManager
的 peak detection 思路）：

```text
每包：stall_peak -= elapsed_ms × decay_rate / 1000        （无 stall 时的线性衰减）
      if (is_stall) stall_peak = max(stall_peak, gap)     （刷新为 max）
```

衰减速率的取舍是"记多久"与"挂多久"的权衡：

- **太慢（小）**：一次大事故把 target 钉在高位很久 —— 延迟债务重；
- **太快（大）**：周期性中小 stall 之间可能衰减过头，下次 stall 前 target 掉得太低，再次欠载。

周期性中小 stall（下载拥塞实测 ≈2.7 次/s、间隔 ≈370ms）之间只衰减约 3.7ms，峰值紧贴近期最坏值 —— 拥塞期间 target 保持反应；而单次大
stall 的"钉高位"时长有界可解释（170ms 事故衰减到与 `k×J` 的交叉点约 14s， 50ms stall 约 2.4s）。`decay = 0` =
峰值永久保持（把"近期最坏"变成"历史最坏"，用于复现钉高位症状）。

### 4.4 transit / base_delay / 乱序（诊断量）

- **transit**：相对锚点的差分，首包建立 anchor 以消去 timestamp 的随机 offset。只用于诊断路径漂移。
- **base_delay**：transit 的累积最小值 = 路径底噪的简化口径（长期单调漂移下只会偏低）。 **不进控制律**（ADR-1）。
- **seq 分类**：滑动 64 位窗内已见 = duplicate；窗内未见但落后 = reordered；窗外 = late。 落后包 **不更新** transit / J /
  到达基线（RFC 口径只对按序到达做 J，落后包的到达间隔为负会污染统计）。
- **timestamp 非单调** = 发送端时间轴重置：重建 transit 锚点并跳过本次 J 更新，不触发任何 JB 动作。
- **SSRC 变化** = 新流：全部重置后按首包处理。

### 4.5 尾部直方图 P99（抖动项的默认输入）

- **量**：单包绝对偏差 `|到达间隔 − 发送间隔|`（差分天然漂移不变；刻意不用
  `transit − base`，累积最小 base 在持续漂移/阶跃下永不更新、相对值会钉死高位）。
- **结构**：近 2048 包（≈7.5s）滑动窗口 + 64 桶（1ms/桶，≥64ms 进溢出桶），
  环 + 增量计数，P99 每包 64 桶线性扫。strand 内 O(1)/零分配/无锁，对外只经原子发布。
- **语义**：均值噪声碰不到 P99（2000 个包里排前 1% 才搬得动它）；真恶化时新极端
  一进来就是 P99。涨快跌慢内建：涨 = 新样本，跌 = 等旧样本滑出窗口。
- **冷启动门**：128 包（≈0.47s）前发布 −1，controller 回退 k×J——窗口未满时
  P99 ≈ 最大值，启动期一次断流能把 target 一步顶到顶。
- 只进走到 transit 计算的包（按序 + 时间轴有效）；乱序/迟到/重置包不进；
  stall 包也进（它的 |D| 是合法尾部样本，只是密度不到 1% 时 P99 看不见，
  由 stall 峰值项负责它）。

## 5. 决策层控制律（TargetController）

### 5.1 完整公式

```text
desired       = ceil( clamp( margin_slots, effective_min, max_target ) )
margin_slots  = max( 抖动项, min( stall_peak / packet_ms + 1, stall_peak_cap ) )
  抖动项（默认 TailQuantile） = tail_P99 / packet_ms + 1
         （冷启动 128 包内无数据 → 回退 k×J；ScaledJitter 策略恒用 k×J）
effective_min = max( --jb-min-target, 几何地板 + 1 ) + penalty     （夹到 max_target）
max_target    = 2/3 × capacity                                      （结构性，非旋钮）
```

| 项                                   | 来源                                            | 作用                                                                                                      |
|--------------------------------------|-------------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| 抖动项（默认 `tail_P99/packet_ms + 1`） | estimator 尾部直方图（§4.5）                  | **主力预测项**：尾部运动才搬家，均值噪声碰不到；`+1 包` = 挺过最坏到达后水位不归零 |
| 抖动项（legacy `k × J / packet_ms`）  | estimator 的 J × `--jb-jitter-gain`             | 冷启动回退 + 影子镜像；干净链路 `J → 0` 时退到地板，不会过度缓冲                       |
| `min(stall_peak/packet_ms + 1, cap)` | estimator 的 stall 峰值 × `--jb-stall-peak-cap` | **尾部补丁（有限幅）**：接管被 stall 门剔除的拥塞间隙。`+1 包` = 挺过间隙后水位不归零（留一包垫到达相位） |
| `effective_min` 的地板部分           | `max(--jb-min-target, 几何地板+1)`              | **结构性下限**：几何地板无条件托底（ADR-3）                                                               |
| `effective_min` 的 penalty 部分      | `--jb-underrun-penalty` + 累计上限 + 回落速率   | **闭环安全网**：预测项覆盖不了随机尾部与丢包，反馈项补这个洞                                              |
| `2/3 × capacity`                     | `--jb-capacity`                                 | **结构上限**：上 1/3 留给抖动吸收（ADR-2）                                                                |

**为什么两项取 max 而不是相加**：两者都是"需要多少水"的估计，stall 的亚阈值残余本来就在 J 里，相加会重复计。

**为什么惩罚抬下限而不是加到 margin 上**：`k×J` 已经很高时不该重复叠加，而 `k×J` 失算（随机尾部 / 丢包）时 下限才真正起作用。

**几何地板的口径**：构造期用 start 时的请求帧数（`frames_per_buffer`）估算；playback 启动后 `ClientRuntime`
以 `pull_playback` 观测的 **实际** callback 帧数为准（RT 线程原子缓存、监督线程 500ms 轮询比对，变化即
`update_geometric_floor()` 校正）。backend 自选周期（`frames_per_buffer = 0`）或设备切换事务改变 callback 几何时自动跟进，不需重启会话。

### 5.2 迟滞：涨快跌慢 + dwell + 风暴冻结 + 无死区

| 机制     | 行为                                                           | 为什么                                            |
|----------|----------------------------------------------------------------|---------------------------------------------------|
| 涨       | **恒即时**（超死区立即跟进到 desired，限速余量清零）           | 恶化必须立刻跟进，不允许延迟                      |
| 跌       | 按 `fall_rate` 限速（槽/秒，离目标≥4 槽时 4 倍速），小数余量跨拍累积 | 恢复要缓慢，防止在槽边界摆动时 target 来回抽动；远距加速治风暴过后的 overhang |
| 涨后锁跌 | dwell 窗口内**禁止下跌**，且不攒限速余量（否则窗口一过会跳变） | 峰值保持：摆动期把 target 钉在较高值            |
| 风暴冻结 | stall≥1 个/秒时**冻结一切下跌**（涨仍即时），连续完整 2s 窗口零事件才退出 | 断流频发期逐个追 spike 只会上蹿下跳，端稳等风暴过去 |
| 死区     | **恒定 0**（ADR-4）                                            | 它不是阻尼机制，与跌侧限速叠加会产生永久偏移      |

判读要点：稳态下 target 不动时，"desired 真的等于 current"与"被 dwell / 限速 / 风暴按住了"必须可区分 ——
`TargetController` 在每次 `update()` 结算一个 `path`（`steady` / `rise` / `fall` / `dwell_lock` / `storm_hold` /
`deadband` / `no_time_base`）并对外提供 `last_desired()`；日志用法见 `modules/observability.md`。

### 5.3 欠载反馈闭环

```text
发生欠载（underrun_events 增量）→ penalty += Δ × per_event      （封顶 max_slots）
无新欠载                          → penalty -= Δt × decay_rate   （下限 0）
penalty 用 static_cast<uint32_t> 取整后加到 effective_min
```

- 计数器的 **单调性**依赖 JB 的 `underrun_events`；计数器倒退只可能来自 JB reset（新会话），按"无新欠载"处理， 不产生负增量；
- 干净链路上 `penalty` 恒为 0，不增延迟；
- `per_event = 0` 关闭整条反馈闭环 —— 分离"预测项"与"反馈项"各自贡献的关键对照。

### 5.4 margin 策略与影子对照组

抖动项有两档实现，经 `TargetMarginStrategy` 选择（产品默认 `TailQuantile`，
`--jb-margin-strategy legacy` 可切回；组件默认仍是 `ScaledJitter`）：

| 策略 | 抖动项 | 适用 |
|---|---|---|
| `TailQuantile`（默认） | P99/包周期 + 1（§4.5；冷启动 128 包内回退 k×J） | 稳态：均值噪声碰不到 P99 |
| `ScaledJitter` | k×J（转正前逐字行为） | 逃生舱 + 冷启动回退 + 单测稳定 |

无论哪档，**影子 desired**（`leg=` 列）恒按 legacy k×J 路径另算一份、
同样的夹持路径，**只看不碰**：current 稳而影子晃 = 分位数在干活；
反之 = 分位数漏东西，回来报告。

## 6. 执行层联动

决策层只写 `target_slots_`，执行层按 target 现算水位带并响应。以下只写 **联动关系**，算法细节在 `buffer_design.md`。

- **水位带随 target 等比缩放**：`wl / nl / nh / wh` 由构造期预计算的带表按 target 查表得到，保持与固定模式相同的
  band/target 倍率。 **不能只改 target**：JB 的 config 校验强制 `warning_low < normal_low < target < normal_high <
  warning_high`，只把 target 折小会低于 `normal_low` 而被拒绝（自适应模式直接起不来）。
- **FILL / DROP**：`lead < normal_low` → FILL（慢放重复当前槽）；`lead > normal_high` → DROP（跳槽追上）。
  `warning_high` 以上是 deadline-high DROP（跳步最大）；`warning_low` 以下是强制静音 Hold（时间轴修正）。
- **reanchor**：由 producer 在"远超前距离 ≥ capacity 且缺口 ≥ min_gap"时探测、consumer 在安全时机应用； hold-stuck
  兜底防止"等 target 等不到"变成永久静音。
- **concealment**：`lead` 排空或槽缺失时重复上一个真实包 + 线性淡出，超过连续上限转静音。
- **splice crossfade**：DROP 着陆 / FILL 重播 / 掩盖进出 / 进出静音 / reanchor 后静音共 7 个拼接点，
  后续输出前 64 帧（≈1.33ms）从最后一个已播采样线性淡出。只改输出样本值，不动时间轴与 target；
  出问题关 `cfg.splice.enabled` 一行（`--jb-no-splice`）。
  可观测性：`JB::splice_events()` 是唯一的 arm 漏斗计数（只在真正 arm 时自增），
  CLI 诊断渲染成 `jb{... splice=N/s ...}` 的速率——"现场是否真的在拼接、有没有连发"
  从此不靠耳朵；离线侧由 `jitter_control_replay_test` 的 splice 开/关对照量化。

## 7. 不变式与验收口径

### 7.1 不变式（破坏即 bug）

1. **三时钟域职责不变式**（第 2 节表）：跨域只经原子快照；RT 线程不写 controller，controller 不做 IO/分配。
2. **几何地板无条件托底**：`effective_min ≥ 几何地板 + 1`，任何旋钮只能抬高不能压低。
3. **结构上限不可顶穿**：`target ≤ 2/3 × capacity`。理由见 ADR-2（顶穿的实测后果）。
4. **水位带严格序**：`wl < nl < target < nh < wh`（整除后 `capacity = 4` 时 `wl == nl` 是已知的退化，行为安全）。
5. **JB 接受/拒绝语义与 target 无关**：远超前检测（"是否请求 reanchor"）与"是否接受该包"有意分离。

### 7.2 验收口径（观察指标，不是不变式）

| 指标                      | 口径                                      | 说明                                                                                                   |
|---------------------------|-------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `underrun_ratio`          | `underrun_frames / pull_frames`           | 预算 `< 0.1%` **只对无损无 stall 的稳态成立**；有丢包/断流时它是"链路本来就在丢"的度量，不是控制器缺陷 |
| `max_underrun_run`        | 单次最长连续缺帧槽数                      | **是观察指标，不是不变式**：它是"这次事故有多长"的描述量，不设通过阈值                                 |
| `fill_duty` / `drop_duty` | Fill 慢放多播帧 / Drop 跳过槽的帧占比     | 判"真振荡"看这两个，不看 target 方向反转次数（后者会误报）                                             |
| `busy` 拒绝率             | `push_rejected_slot_busy / push_accepted` | 顶穿结构上限的直接证据；正常应为 0                                                                     |
| 听感                      | concealment 是否盖过静音                  | `--jb-no-conceal` 做 A/B                                                                               |

## 8. 参数手册

**数值一律见 `configuration_reference.md`；每个常量的物理含义、调整方向与实测依据写在
`aqua_core/include/aqua/audio/buffer/buffer_config.h` 的就地注释里。** 本节只给分类与判据。

### 8.1 CLI 旋钮（有明确"延迟 ↔ 稳定"权衡）

| CLI                     | 调它的理由                                         |
|-------------------------|----------------------------------------------------|
| `--jb-capacity`         | 主刻度：越大越抗抖动、稳态延迟越高。下限是结构性的 |
| `--jb-jitter-gain`      | legacy k×J 路径的 k（默认策略下只影响冷启动回退与影子列）。主力预测项地位已让给尾部分位数 |
| `--jb-min-target`       | 抬高最低延迟。只能抬高（几何地板无条件托底）       |
| `--jb-margin-strategy`  | `tail`（默认）/`legacy` 切换抖动项公式。逃生舱：野外证明尾部不如均值时切回对照 |
| `--jb-stall-peak-cap`   | stall 峰值项最多把 target 推多高。0 = 关闭该项     |
| `--jb-stall-decay`      | 峰值记多久。0 = 永久保持                           |
| `--jb-stall-threshold`  | stall 门阈值。≤0 = 关检测（裸 RFC 3550）           |
| `--jb-underrun-penalty` | 每次欠载抬升下限的槽数。0 = 关闭反馈闭环           |

### 8.2 开关（非旋钮）

| CLI                 | 作用                                                                                   |
|---------------------|----------------------------------------------------------------------------------------|
| `--jb-fixed-target` | 关自适应，回固定 `target / startup` 比例。A/B 对照基线（打开时 controller 根本不创建） |
| `--jb-no-conceal`   | 缺帧直接静音，不走 repeat-last + 淡出                                                  |
| `--jb-no-splice`    | 回到整包硬拼接。crossfade 是否涂抹瞬态的 A/B（只改输出样本，不动控制）                 |

### 8.3 已内部化（无 CLI 入口）

这批只有一个很窄的合理区间，暴露出去只会制造误调：起步 target、回落限速（含远距加速倍率）、涨后 dwell、惩罚的累计上限与回落
速率、 reanchor
的跨度上限 / 最小缺口 / hold-stuck 阈值、warning 步长曲线、concealment 连续上限、尾部窗口 / 桶数 / 分位数 /
冷启动门、风暴阈值与窗口、splice 混合长度。要改就改
`buffer_config.h` 重新编译。

### 8.4 结构性约束（不是旋钮，不要动）

`JB_ADAPTIVE_TARGET_CAPACITY_RATIO`（2/3）、`JB_ADAPTIVE_DEADBAND_SLOTS`（恒 0）、
`JB_MIN_CAPACITY_SLOTS`（4）、`JB_MAX_CAPACITY_SLOTS`（512 护栏）、`JB_ESTIMATOR_REORDER_WINDOW_PACKETS`（64， 结构决定）。理由见
`configuration_reference.md` 与第 11 节 ADR。

### 8.5 候选清单（暂不暴露，出现明确需求再加）

`JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC`（恢复速度）与 `JB_ADAPTIVE_RISE_DWELL_MS`（峰值保持窗口）有价值， 但当前只在"恢复太慢 /
target 抽动"类问题上才会想动；旋钮一次加太多会让归因变难。真遇到再加， 加的时候照第 8.1 节的模式同步 C API 与文档。

## 9. 调参与排障 playbook

完整"症状 → 先看哪行日志 → 动哪个旋钮"表在 `modules/observability.md`。这里给决策顺序与按环境的推荐起点：

1. **先默认跑**，看 `target p50/p95`、`underrun_ratio`、`drop_duty`、听感；
2. **先分清缺口的性质**：`JitterEstimator stall:` 刷屏说明链路在断流 —— 那不是 target 能救的；
   `network stall` 与 `stall_peak` 一起看，判断是"抖动超出预测"还是"stall 尾部被门剔除"；
3. **再动对应的一项**：抖动看尾部 P99（`leg=` 列对照 legacy），冷启动行为看 `--jb-jitter-gain`；stall 尾部看 `--jb-stall-peak-cap` / `--jb-stall-decay`；
   反馈是否在起作用 → `--jb-underrun-penalty 0` 对照；怀疑分位数本身 → `--jb-margin-strategy legacy` 对照；
4. **最后才动容量**：`--jb-capacity` 是延迟与内存的主刻度，改它会同时改变结构上限与整组水位带。

### 9.1 按网络环境的推荐起点

**默认值是按"家用 Wi-Fi + 同链路有下载"定标的**。其它环境从下表的组合起步， 跑 5~10 分钟看三个验收指标——
`underrun_ratio < 0.1%`、`drop_duty < 0.5%`、`target p95` 稳定不抽动—— 不达标再按上面四步微调。 换算基准： **默认档（48kHz /
stereo / F32）1 槽 = 3.646ms（F=175）**，几何地板 = 4 槽 ≈ 14.6ms （Windows WASAPI shared 的 480 帧 / 10ms callback）。⚠️ **F
随会话格式变化**（auto-F = MTU 预算与 5ms 时长取小），换格式时每槽毫秒数要按 `F / sample_rate` 重算，不要死记 3.646。

> **本节数据已按"发送端 pacing 修好之后"的环境更新**：现在发送端按 packet 周期摊平发包
> （见 `modules/server_audio_path.md`），稳态 J 从 3~5ms 降到 <1ms，剩下的到达异常几乎全是
> **stall（时间断流）**而不是抖动。因此无线场景的主旋钮是"盖住 stall 分位数"，不是放大 k。

#### 有线 LAN（同网段 / 机房直连）

```text
（全部默认，不需要任何 --jb-* 参数）
```

J < 1ms、stall ≈ 0：margin 两项都很小，target 被有效下限接住，自然落在几何地板（≈14.6ms）——这已经是
当前几何下的最低安全延迟，再压会被地板无条件托底（ADR-3），压不动。若 LAN 上仍见欠载，那不是 JB 参数问题： 查 NIC
中断聚合、交换机缓存或发送端调度（先看 `catchup_drains`，见
`operations_and_troubleshooting.md` §8.2）。想验证"地板长什么样"可以 `--jb-jitter-gain 0`
（`floor_bind=1` 恒成立，target 钉在地板）。

#### Wi-Fi（家用 / 办公 / 软路由无线）

```text
--jb-min-target 9          # 主旋钮：盖住实测 p99 空隙（≈29ms → 9 槽 = 32.8ms）
--jb-stall-peak-cap 12     # 拥挤 / 常开下载时再加
```

实测样本（软路由 AP，2 分钟 149 次 stall，零丢包零乱序）：空隙 **p50 20.4ms / p90 24.5ms / p99 29.2ms / max 41.7ms**
。选档方法就一句话—— **用 p99 定 `--jb-min-target`，不要用 max**：

```text
槽数 = 想覆盖的空隙时长 / (F / sample_rate)
例：p99 = 29.2ms、packet = 3.646ms → 9 槽 = 32.8ms
```

| 目标                 | `--jb-min-target` | 代价 / 收益                             |
|----------------------|-------------------|-----------------------------------------|
| 低延迟极限           | 6（21.9ms）       | 覆盖 p50；p90+ 的空隙会变成 concealment |
| **稳定优先（推荐）** | **9（32.8ms）**   | 覆盖 p99，欠载接近零                    |
| 恶劣环境             | 12（43.8ms）      | 覆盖更差的分位，固定延迟明显增加        |

> **选档前先离线验证**：`aqua_jitter_buffer_tests --gtest_filter=*ScenarioTable*`（见
> `testing.md` §4.1）按真实控制律给出该链路下 target 的落点、静音占比与最长连续断流，
> 比现场试错快得多；投产前再用 `--jb-trace` + `--log-file` 采一份现场到达节奏回来重放。
> 注意 p99 是**滑窗分位数**：一次 2s 断流会以 stall 峰值项（封顶）的形式挂住约 174s
> （见 `testing.md` §4.2），此时 target 不代表链路常态。

只靠 stall 峰值项（`--jb-stall-peak-cap`）也能把 target 抬到 7~8 槽，但那是"事后补课"： stall 先发生、target 才涨、中间那几次就是欠载与
concealment。 **直接抬 `--jb-min-target`
是把下限预先垫到位**，这才是无线链路上欠载归零的原因。Android 对应 `WIFI_SMOOTH` 档。

#### 公网 / 跨地域（WAN、VPN、4G/5G）

```text
--jb-capacity 60 --jb-min-target 6 --jb-stall-peak-cap 16 --jb-stall-decay 5
```

逐参数作用：

- `--jb-capacity 60`（默认 30）：公网抖动峰峰值大，先买够吸收余量——结构上限随之为 `2/3×60` = 40 槽（146ms），是给 target
  留的上升空间；内存代价 ≈ 86KB，可忽略。
- `--jb-min-target 6`（出厂默认 3；有效下限 = max (6, 地板+1) = 6 ≈ 21.9ms）：WiFi 日常 20~50ms 断流下 4 槽地板太薄， 与其每次欠载后靠
  penalty 一槽一槽补，不如直接把地板抬到位——它只抬下限，不影响上限。
- `--jb-stall-peak-cap 16`（默认 8 = 30ms）：公网 50~60ms 档的到达间隙是常态而非事故，cap 16 ≈ 58ms 让
  峰值项对这档继续线性响应，而不是饱和后全推给反馈闭环。
- `--jb-stall-decay 5`（默认 10）：公网 stall 稀疏、间隔大，衰减慢一半才能在两次 stall 之间仍"记得"
  上次的教训，避免每次回落到底再重新交学费。
- `--jb-jitter-gain` 保持 5：公网的 J 均值本身已经变大，`k×J` 会自动抬高 target，不需要额外放大 k。
- concealment 保持开（默认）：公网必有真丢包，repeat-last + 淡出比硬静音耐听得多。

#### 高丢包 / 弱网（移动网络边缘、拥塞 AP）

```text
--jb-capacity 80 --jb-min-target 8 --jb-stall-peak-cap 16 --jb-stall-decay 5 --jb-underrun-penalty 2
```

在公网组合的基础上：

- `--jb-underrun-penalty 2`（默认 1）：单包丢失只把到达间隔拉到 ~2 个包周期，远低于 stall 门（5），
  峰值项对散点丢包基本失明——只能靠欠载反馈闭环补偿；步长翻倍让下限抬升跟得上丢包率，累计上限 6 槽 封顶，不会失控。
- `--jb-capacity 80` / `--jb-min-target 8`：loss 与 reorder 都需要更深的环来吸收；若日志里 `cap_bind=1`
  成为常态，说明 target 长期贴结构上限，继续加 `--jb-capacity`。

#### 低延迟优先（游戏语音、实时连麦）

方向相反：先 `--jb-capacity 16`（结构上限压到 10 槽 ≈ 36.5ms），`--jb-min-target` 保持默认让 target 有 下探空间，
`--jb-stall-peak-cap 4`（15ms，stall 尾部只买最小保险）。这是 **用抗抖动换延迟**——
`underrun_ratio` 上升到 0.5% 以内、靠 concealment 兜住听感即达标；超过则说明该链路配不上这个延迟目标， 回到上一档。

## 10. 实验复现矩阵

每一行是一个可独立复现的 CLI 组合，用于分离控制律各项的贡献或复现已知症状。全部以双机直连 + `--log-level debug`

+ 两个调试宏开启的 debug 构建为基线，观察对象是 `TargetController` / `JitterEstimator` 日志与 1s diag 行。

| #   | 目的                         | CLI 组合                                        | 预期与判读                                                               |
|-----|------------------------------|-------------------------------------------------|--------------------------------------------------------------------------|
| E0  | 基线                         | `--jb-jitter-gain 5`                            | target 在 4~7 槽区间随 J/stall 浮动，`busy = 0`，`underrun_ratio < 0.1%` |
| E1  | 关自适应（固定模式对照）     | `--jb-fixed-target`                             | controller 不创建，无 `TargetController` 日志；target 恒为构造比例值     |
| E2  | 只要预测项                   | `--jb-stall-peak-cap 0 --jb-underrun-penalty 0` | `src=kJ` 恒成立；stall 后 target 不再被峰值项抬起                        |
| E3  | 只要峰值项                   | `--jb-jitter-gain 0`                            | 干净链路 target 落到地板（`floor_bind=1`）；有 stall 时 `src=stall_peak` |
| E4  | 只要反馈项                   | `--jb-jitter-gain 0 --jb-stall-peak-cap 0`      | 欠载前 target 贴地板，欠载后按 `penalty` 逐步抬升                        |
| E5  | 关 stall 剔除（裸 RFC 3550） | `--jb-stall-threshold 0`                        | 每次断流的间隔都进 J，J 与 target 被历史事故抬高后缓慢回落               |
| E6  | 峰值永久保持                 | `--jb-stall-decay 0`                            | 一次大 stall 后 target 长期钉在高位（复现"钉高位"症状）                  |
| E7  | 峰值为"历史最坏"             | `--jb-stall-peak-cap 100 --jb-stall-decay 0`    | 极值：target 直接被推到结构上限，观察 `cap_bind=1` 与 `busy`             |
| E8  | 峰值项不设上限               | `--jb-stall-peak-cap 100`                       | 孤立大 stall 买入大额延迟债务 → 随后 DROP 还债（`drop_duty` 冲高）       |
| E9  | 关掩盖（听感对照）           | `--jb-no-conceal`                               | 缺帧直接静音；A/B 判断 repeat-last 是否盖过静音                          |
| E10 | 更保守                       | `--jb-jitter-gain 8 --jb-min-target 6`          | 延迟换稳定：`underrun_ratio` 下降、`lead` 抬高                           |
| E11 | 更激进                       | `--jb-jitter-gain 2 --jb-min-target 3`          | 延迟下降，干净链路欠载应仍为 0（地板托底）                               |
| E12 | 结构性上限的实测证据链       | `--jb-jitter-gain 100`                          | target 顶到 `2/3 × capacity`（`cap_bind=1`）；继续加大 gain 不再涨       |

## 11. 设计决策记录（ADR）

每条只写"为什么这么定、否决了什么、有没有实测证据"。

**ADR-1：base_delay 不进控制律。**
estimator 的 base 是 anchor 相对的 transit 累积最小值，构造上恒 ≤ 0（首包 anchor 使 transit 起点为 0，只取 min）， burst
发包下实测 -6 ~ -15ms，正贡献路径是死代码。target 的物理意义是"该持有多少已到达的数据"，不是网络单程延迟， 把底噪算进 buffer
budget 是概念错误。base / transit 保留在诊断输出（观察路径漂移仍有价值）。

**ADR-2：目标上限是 `2/3 × capacity` 而不是 capacity 本身。**
水位带随 target 等比放大（`warning_high ≈ 1.5 × target`）。target 顶到 capacity 时整条高水位带落到 ring 之外、 DROP
机制失效；consumer 把 lead 顶满 ring 后新到的包全部撞上未消费槽位被 slot-busy 拒收——人为造洞，consumer 再在洞上欠载。
**实测（极端 gain）：`busy ≈ 25% rx`、`underrun ≈ 30%`，UDP 零丢包但声音全破。** 上 1/3 留给 抖动吸收，与固定模式的 0.6
target / 0.9 ceiling 是同一结构。

**ADR-3：几何地板无条件托底，`--jb-min-target` 只能抬高。**
一次 playback callback 消耗 `ceil(callback_frames / F)` 个包；`target ≤ 地板` 意味着"每个 callback 必然把 JB
抽空"，这是结构性不可能，与抖动无关。 **实测：2 槽在 F=175（3.646ms）链路上正好落在欠载悬崖之下（16.6% 欠载 + 25% 丢帧），3
槽归零。**
因此有效下限 = `max(--jb-min-target, 地板 + 1)`，旋钮压不下去。

**ADR-4：死区恒为 0。**
死区与"跌侧不限死区、一路 grind 到底"叠加会产生 **永久偏移**：跌到 desired 后，desired 回升量 ≤ deadband 被吞掉， target
永远停在 desired−1（实测 target 卡 2 而 desired = 3，对应 16.6% 欠载 + 25% 丢帧）。阻尼由跌侧限速提供， 不在这里加死区。

**ADR-5：stall 峰值项加 cap（soft/hard 两区制），而不是只靠衰减。**
stall 是"已经发生的恢复风险信号"，不是新的 steady-state 延迟要求——一次 60ms+ 的孤立 stall 不该买入十几槽的 延迟债务。
**实测：59.6ms stall → target 7→17，随后 17 次 DROP / 49 skip_slots 的还债风暴。** 加上限后
`min(峰值/包周期 + 余量, cap)` 线性增长再饱和，本身就是 soft/hard 两区制，不需要第二个显式阈值；
衰减速率决定"封顶后的高位挂多久"，cap 决定"最多推多高"，两者分工不重叠。

**ADR-6：stall 门阈值取 5 个包周期。**
burst 发包的正常串间间隔约 2.7 个包周期，留近一倍余量；35ms 级及以上的真 stall（双机实测 35~170ms）稳稳落在
线外。阈值必须严格大于串间间隔，否则会把正常发包误判为 stall。`≤ 0` = 关检测，作为"stall 剔除到底有没有用"的 唯一 A/B 手段保留。

**ADR-7：欠载反馈抬下限，不加到 margin 上。**
`k×J` 是均值型预测，覆盖不了随机抖动尾部与丢包；反馈项补这个洞。抬下限而非加 margin 的理由：`k×J` 已经很高时
不重复放大；而预测项失算时下限才真正起作用。它是安全网不是主力，干净链路上恒为 0。

**ADR-8：concealment 只做"整包重复 + 线性淡出 + 静音封顶"，不做 late 插入。**
连续掩盖上限 3 包（≈10.9ms @F=175/48kHz）：再长就是"重复音"而不是"掩盖"，听感比静音更糟。late 包继续 drop，只记
`late_useful_packets` 潜力（落后播放头不超过 conceal 窗口 = 到达时对应槽还在被掩盖，插入即有用）——本阶段不改变
播放时间线，把"要不要接 late"留给数据决定。

**ADR-9：controller 事件驱动，不加 timer。**
见第 2 节推论 1：真断流期间没有可控对象，冻结的状态是对网络的最后可用估计；按墙钟继续衰减会丢掉这份信息。 恢复后第一个包立即刷新。

**ADR-10：诊断必须能回答"target 为什么不动"。**
只打"变化量"的日志无法区分"desired 真的等于 current"与"被 dwell/限速按住"，7↔8 抽动类问题就卡在这里。 因此 controller 结算
`path` 与 `last_desired()` 并做 **事件驱动 + 稳态节流**双档日志；实现与点位见
`modules/observability.md`。

## 12. 已验证的边界与已知限制

**已验证（有实测证据）**：

- 干净有线链路：`J` 收敛后 target 落到地板附近，`underrun_ratio` 归零；
- 下载拥塞（周期性中小 stall ≈2.7 次/s）：峰值项保持反应，target 稳定在 6~8 槽，`underrun_ratio ≈ 0.001`；
- 单次大 stall：峰值项经 cap 限幅，target 上冲有界，欠载由 penalty + concealment + reanchor 分段接管；
- 极端 gain：被 `2/3 × capacity` 接住，`cap_bind = 1`，不失控（但此配置本身是病态，见 ADR-2）。

**已知限制**：

- `underrun_ratio < 0.1%` 的预算只对无损无 stall 稳态成立；有丢包/断流时它是链路度量，不是控制器缺陷；
- `capacity = 4` 时整除后 `warning_low == normal_low`，阶梯填充带消失，`decide` 退化为更积极的 hold-fill （行为安全，但不是设计意图）；
- 断流期间 controller 状态冻结（ADR-9 的推论）：若断流长到绕过 concealment 与 reanchor 的兜底能力， 短暂静音是预期行为；
- AAudio 后端无 `frames_per_burst` 语义，几何地板依赖 `pull_playback` 的实际 callback 观测校正， 首回调到达前的起步窗口用的是请求值口径。
