# Diagnostics

CLI `server` / `client` 每秒打印一行诊断（`--log-level debug`/`trace` 才输出），
形状如下（实际是一行，这里折行以便阅读）：

```text
Server diag: state{...} audio{...} capture{...} pktz{...} queue{...} dsp{...} net{...} sess{...}
Client diag: state{...} net{...} jb{...} jc{...} pb{...} stream{...}
```

每个诊断量按**模块**聚合成紧凑块，外层由 `SnapshotLine` 包成 `module{...}`。
块内是 `k=v` 空格分隔的键值对；块名与渲染方法见 第8节，各字段速查见 第7节。
读行时的直觉顺序：**state**（整体活着吗）→ **net**（链路通不通、有没有丢/错）→
**jb**（缓冲水位/目标/重锚定/欠载，音质第一现场）→ **jc**（目标为什么是这个值）→
**pb** / **stream**（播放侧是否真的在出声、有没有 xrun）。

> 这一行只在 **log level = debug / trace** 时打印。生产/Release 现场若只看
> info 及以上，看不到它——但 Android 侧的 JNI 诊断（读同一份快照）始终可用。

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

边界约定：

- **首拍**没有基线，显示 `T/0/0.0`（delta、rate 记为 0）。
- 计数器**回退**（当前 total 小于上次，极少见）时 `D` 记 0，避免出现负速率。
- `R` 用真实 `steady_clock` 间隔计算，不假定定时器精确为 1.000s。

纯**仪表**（gauge）字段（水位、队列深度、延迟毫秒、枚举名等）直接显示值，
没有 `/`。部分仪表带单位或精度后缀，例如 `age=232ms`、`tgt=4(14.6ms)`、
`uratio=0.000045`、`fduty=0.000000`。

CLI 侧这套 `T/D/R` 由 `RateCounter`（见 第8节）维护；`SnapshotLine::log_debug()` 只负责把各块拼成一行。 **Android 侧等价逻辑在 Kotlin 层**（`AquaRates.kt` 的
`RateSampler`）： 诊断快照本身仍是"无时间状态的聚合快照"（契约不变、槽位不变），App 对相邻两次采样做差分、除以真实 elapsed 得到
/s， 主页卡片把它作为累计值下方的一行展示——累计值看不出"此刻是否在恶化"，速率才看得出来。两处只在 **拿到新诊断**时采样
（Android 侧诊断刷新约 1s 一次），计数器回退（重连后从 0 重计）时该拍跳过，避免算出负速率。

## 2. Debug gating

`SnapshotLine::log_debug()` 先判断 Debug 是否启用；未启用时连 source 都不调用。这一点很重要：诊断 getter 本身可能跨多个
atomic 读取，如果用户不看 debug 日志就不应该为它付成本。

RT 路径的调试日志开关 `AQUA_SERVER_RT_DEBUG_LOG` / `AQUA_CLIENT_RT_DEBUG_LOG`（默认关；Debug 构建预设显式开启、
Release 预设关闭）一旦开启会在 server 音频实时链（采集/packetizer/dispatcher）与 client `pull()/decide()`、
播放回调中同步调用 spdlog， **破坏严格 RT 契约**，仅用于短时间问题复现。

决策层的调试日志开关 `AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG`（同样默认关、debug 预设开）覆盖 estimator /
controller / push strand 的判定日志。它运行在非实时线程上，不影响音频实时性，但会给每包处理路径加上带锁的
格式化，同样只用于短时间排查。三个宏的边界、点位全表与"看到某行日志该去哪查"的排查表见 `modules/observability.md`。

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
- 接收序列缺口（`rx_audio_sequence_gap_events` / `missing_frames`：对端丢包或重排的规模）
- 观测层（`JitterEstimator`）：`jit_ms` / `base_ms` / `transit_ms` / `reordered` / `duplicate` / `late`
- 欠载与掩盖：`underrun_events` / `underrun_frames` / `underrun_ratio` / `max_underrun_run`、
  `concealed_slots` / `concealed_saturated_slots`、以及迟到但有用的包 `late_useful_packets`
- playback callback pull 统计

### 4.1 决策层观测（`jitter_control` 组）

上面那批是"结果与累计计数"；这一组是"决策与阈值"，回答 **为什么** target / 水位 / 掩盖是现在这样。三部分来源不同：

| 来源                                     | 字段                                                                                                                                                                                             | 用途                             |
|------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------|
| TargetController（决策层，仅自适应模式） | `adaptive`、`desired_slots`、`min_slots`、`max_slots`、`margin_source`、`path`、`floor_bound`、`cap_bound`、`underrun_penalty`、`dwell_remaining_ms`、`fall_room_slots`（另有 `geometric_floor_slots` 只供 CLI/离线诊断，不进 C 边界，App 侧不可见） | target 为什么涨 / 跌 / 不动      |
| JitterEstimator（观测层尾部）            | `stall_events`、`stall_peak_ms`、`last_stall_gap_ms`、`arrival_interval_ms`                                                                                                                      | 断流侧：门剔除次数与近期最坏间隙 |
| JitterBuffer（执行层）                   | `band_warning_low`/`band_normal_low`/`band_normal_high`/`band_warning_high`、`conceal_run_slots`、`underrun_run_slots`                                                                           | 阈值与"此刻在发生什么"           |

判读入口（三个最常用的组合）：

- `desired_slots != target_slots` → 本拍 target 被限速 / 涨后锁跌 / 风暴冻结按住，看 `path` 与 `dwell_remaining_ms`；
- `floor_bound=1` → 算出来的余量不够、靠下限（几何地板或欠载反馈）托住；`cap_bound=1` → 顶到 2/3 结构上限；
- `stall_peak_ms` 大而 `estimator_jitter_ms` 小 → 缺口是"断流尾部被门剔除"，该调 stall 峰值上限而不是 k。

字段语义与调整方向见 `jitter_buffer_control_design.md` 第5节；日志侧的对应点位见 `modules/observability.md`。 Android 侧这一组经
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
排障设备问题时以前者为准，见 `operations_and_troubleshooting.md` 第6节。

## 6. 快照口径

`ServerDiagnosticsSnapshot` / `ClientDiagnosticsSnapshot` 是一次性聚合：各字段为独立的原子近似读，跨字段不保证同一时刻一致，
仅供监控与显示， **不用于控制决策**。

两处容易误读的口径：

- `dispatcher.dropped_frames` 转发的是 `AudioFrameQueue` 的丢弃数，dispatcher 自身没有丢弃计数器；
- session 的 `removed` 已包含 `expired` 与 `removed_by_clear`，三者不能相加求总数。
- 诊断字段数是 **三方契约**：JNI 写入序列（`aqua_jni.cpp` 的 `constexpr build_diagnostics()`）+ C 头
  `AQUA_DIAGNOSTICS_FIELD_COUNT` + Kotlin `AquaDiagnostics.fromArray` 必须同步。前两者由 `static_assert`
  在编译期锁定（加字段忘改常量 = 编译失败）；Kotlin 侧 size 不符会让 `fromArray` 返回 null（UI 停在
  "正在收集数据…"）。JNI 侧是"填满 C++ 数组后一次 `SetLongArrayRegion` 提交"，不是逐字段写数组。
- 音频错误通道（`last_audio_error` / `audio_error_epoch`） **不在快照内**：两者打包进同一个 64 位原子 （低 8 位 =
  错误值，高位 = epoch），一次 CAS 发布，读方不会看到"新错误 + 旧 epoch"。
- 渲染层（`SnapshotView` / `FieldBlock`，见 第8节）只服务于 CLI 日志，不影响 C API 契约：
  Android 侧按自己的逻辑从累计计数器算速率（`AquaRates.kt` 的 `RateSampler`）：
  诊断快照本身仍是"无时间状态的聚合快照"（契约不变、槽位不变），App 对相邻两次采样做差分、除以真实 elapsed 得到
  /s，主页卡片把它作为累计值下方的一行展示——累计值看不出"此刻是否在恶化"，速率才看得出来。两处只在 **拿到新诊断**时采样
  （Android 侧诊断刷新约 1s 一次），计数器回退（重连后从 0 重计）时该拍跳过，避免算出负速率。

## 7. 渲染字段速查（CLI 行）

块内键名走「短而稳定」路线。`T/D/R` 表示该字段是 第1节 的速率三段式；未标注的即仪表值。
列可能随版本增减（以 `SnapshotView::render_*` 实现为准），这里只解释含义。

### Server

**state**：`state` 运行状态，`sess` 活跃 session 数，`udp` 数据面端口。

**audio**（采集链路总体）：`capture` 采集是否运行，`err` 最近音频错误，
`fmt` 音频格式（`2ch/48k/enc3`，Hz 整除 1000 简写为 k），`F` 每包帧数，
`src` 采集源（0=input，1=loopback），`cstate` 采集状态，`sw` 切换状态，
`route` 路由模式，`dev` 指定设备 ID（仅 route=preferred 且指定时出现），
`ls` 最近切换结果/耗时，`cf`/`cb` 累计采集帧/字节，
`pkt` 每包帧数 last/min/max，`stv` 当前/最大 starved 毫秒。

**capture**（采集回调层）：`ev` 音频事件，`pq` packet 查询，`pe` 空 packet，
`pr` 就绪 packet，`gb` 取缓冲成功，`cbk` 回调次数，`scb` 静音回调，
`ssb` 合成静音块，`gsf` 生成静音帧，`se` starved 事件，`stv_ms` starved 毫秒，
`cf` 采集帧，`cby` 采集字节，`epbuf` 端点缓冲帧数（采集侧延迟的硬地板，
WASAPI 实测 1056 帧 @48k ≈ 22ms），`stale` 事件归属契约丢弃数（非零说明
provenance 过滤真的拦下了东西），`disc` **引擎断流事件数**
（`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`：引擎重建渲染流、**切歌的典型场景**、
xrun 的潜在来源。此前它只有一行 debug 日志且包在 `AQUA_SERVER_RT_DEBUG_LOG` 里
⇒ Release 构建完全看不见；它会**成簇触发**，单行日志数不出次数）。

**pktz**（打包器）：`blk` 输入块，`by` 输入字节，`frm` 发出帧，
`unal` 对齐错误拒绝，`disc` 待丢弃。
注意 `blk`/`by` 在对齐校验**之前**计入，**含随后被 `unal` 拒绝的块**；
所以 `by` 与 `frm × slot_bytes` 的差值里混着被拒块的字节，
不能拿它推算丢失量（完整对账口径见 `modules/packetizer.md`）。

**queue**（采集→网络缓冲）：`depth` 当前深度（槽），`hwm` **终身**峰值（自进程
启动单调，不重置），`hwm_r` **最近一个 reap 周期（200ms）内**的峰值，
`backlog_ms` 积压的**时间**维度（depth × 包周期，槽数本身不告诉你延迟），
`acc`/`con`/`drop` 接受/消费/丢弃。
`hwm` 与 `hwm_r` 要一起看：终身峰值在长跑 server 上是"历史"（启动瞬间的突发会
把它钉在那里，此后不再变化），`hwm_r` 才是"最近最深到几"。

**dsp**（网络分发器）：`enc` 编码帧，`bc` 广播帧，`nocl` 无客户端帧，
`ef` 编码失败，`df` 分发失败，`drop` 丢弃（**口径 = handoff 队列满丢**，
与 `queue.drop` 同一个数字的别名；dispatcher 自己没有独立的网络队列丢包计数），
`pub` 发布，`wake` worker 唤醒。
pacing / 追赶组：`paced` pacing 模式发出的包数，`catchup` **追赶拍次数**
（稳态必须为 0；>0 表示 worker 被调度饿死过，或生产瞬时超过了发送能力——
client 侧会看到周期性缺口），`catchup_on` 此刻是否正在追赶（逐拍状态量），
`period_ns` 有效包周期（ns，0 = pacing 关闭），`cu_depth`/`cu_low`/`cu_speed`/`cu_max`
追赶阈值与加速倍数（编译期常量，恒已知）。**给出阈值才能解读 depth**：
6 到底是高于还是低于 8 的追赶深度？

**net**（传输层）：`served` 本实例在服务哪几族数据面（`v4+v6` / `v4` / `v6` / `none`），
`rx`/`tx` 收发包（**两族合计**），`rxB`/`txB` 收发字节，`rxerr`/`txerr` 收发错误
（`rxunr` 单独计 ICMP 不可达噪声，不计入 `rxerr`，见 `modules/udp_transport.md` 第4节），
`drop` 发送丢弃，`enqf` 入队失败，`q` 队列深度（仪表），`hb_recv` 收到心跳，
`hb_rej` 拒绝心跳，`sess_est`/`sess_ref` 建立/刷新 session，`hb_ack` 心跳 ack，
`mal` 畸形包，`nonhb` 非心跳包。

**两族都在服务时**额外展开逐族字段：`v4rx`/`v4tx`/`v4hb`/`v6rx`/`v6tx`/`v6hb`。这是"哪一族零收包"的唯一
直接观测点（合计口径说不出这件事，而它正是双栈下最可能的故障形状）；单族实例不展开（合计就等于那一族）。
`v4hb`/`v6hb` 是逐族心跳，供对照 `rx` 判断"有包但无合法心跳"，也是收包健康告警（`check_udp_receive_health`）
逐族闸门的同一份数据。

**sess**（session 管理）：`act` 活跃数（仪表），`crt` 创建，`con` 已连接，
`ref` 刷新，`rem` 移除，`exp` 过期，`rmbc` 被清空移除。
活跃度年龄组：`age_n` 有活跃度记录的 session 数，`age_old` 最老那个多久没动（ms），
`age_new` 最新那个多久没动（ms）。`act` 只说"有几个"，不说"它们多久没动"——
`age_old` 接近 session_timeout（默认 5s）说明那个 session 下一轮就会被 reap，
半死的连接在聚合计数里完全看不出来。
`sess_ms` **存活时长**：最老的那个 session 连了多久（ms，距 created_at）。它与
`age_old` 是**两个不同的量**——后者是"距上次控制面 keepalive"，在 1Hz keepalive 下
是锯齿状（0~1s 循环），单看它容易误判成"会话在反复重建"。

### Client

**state**：`state` 运行状态，`route` 播放路由，`ls` 最近切换结果/耗时，
`seq` 切换序号（每次切换事务 +1，比 outcome 更能区分"又切了一次"）。

**net**（命名同 server）：`ack` 心跳 ack，`miss` ack 丢失数（仪表），
`age` 最近 ack 年龄（仪表），`hb_fail` 心跳失败（仪表），`hshk` 握手发送尝试，
`miss_ev` ack 丢失事件，`af` 音频帧接受，`gap` 序列缺口事件，`gap_fr` 缺失帧，
`mal` 畸形，`unsnd` 未知发送方，`wsa` 错误 session 的 ACK，`plm` payload 不匹配，
`nona` 非音频包，`jit`/`base`/`tr` 抖动/基础/中转延迟 (ms)，
`trlvl` transit 慢跟随电平 (ms，1 位小数)、`tstep` 单包 |Δtransit| 超阈值的次数，
`reord`/`dup`/`late` 重排/重复/迟到包。
第二轮（2026-10-19）：`est_pk` estimator 观测包总数（**丢包率的分母**，此前
只有分子），`est_gap`/`est_miss` estimator 自己的缺口事件/缺失包（与上面的
`gap`/`gap_fr` 是两套独立计数，可交叉验证），`base_set` 底噪是否已确立
（消掉"base=0 是没建立还是真为 0"的歧义），`rx_consec` 连续收包错误数
（>0 = 接收路径正在 5→250ms 退避，是直接的延迟贡献项），`rx_ecode` 最近一次
收包错误原始码（"对端发包超大 / 本地收缓冲溢出 / socket 真死"三种处置完全不同，
混在一个 `rxerr` 里分不出来），`udp_fam` 数据面实际族，`sentinel` 服务端是否
通告了通配地址（是 = 数据面复用控制面地址），`learned` **实际对端**
（观测值，可能与通告地址不同；空 = 尚未学到）。

**jb**（抖动缓冲）：`water` 水位 (0~1)，`used` 已用/容量槽，`lead` lead 槽 (ms)，
`tgt` 目标槽 (ms)，`play` 播放序列号，`high` 最高收到序列号，
`reanc`/`req`/`canc`/`san` 重锚定次数/请求/取消/合理性拒绝，
`pend` 是否挂起，`tgtseq` 目标序列，`csil`/`msil` 连续/最大静音帧，
`ep` 当前情节（none/filling/dropping），`push_ok`/`push_rej` 接受/拒绝，
`late`/`busy`/`inv`/`sanity` 迟到/槽忙/非法/合理性拒绝，
`pull`/`pf`/`sil` 拉取次数/帧/静音帧，
`fill_ep`/`fill_sl` 填充情节/修正槽，`drop_ep`/`skip` 丢弃情节/跳过槽，
`splice` crossfade 拼接次数（splice 关时恒 0；看速率判断拼接频率），
`und_ev`/`und_fr`/`uratio`/`max_und` 欠载事件/帧/比例/最大连续槽，
`conceal`/`csat` 掩盖槽/饱和，`fu` 迟到有用包，`fduty`/`dduty` 填充/丢弃占空比。
第二轮（2026-10-19）：`oldest` 缓冲里最旧的已收序列（**第三项序列**——`lead`/
`high` 都基于播放头，没有它分不清 "lead=40 是 40 槽真音频" 还是 "40 槽里全是洞"；
真实占用 = high − oldest + 1），`conceal_on`/`conceal_max` 掩盖开关与上限
（`conceal=0` 时分不清"没触发"还是"功能根本没开"），`splice_on`/`splice_xf`
拼接开关与 crossfade 长度，`startup` 启动预填充槽数，`init_tgt` 启动初始 target。

**jc**（抖动控制层，全部仪表）：`adaptive` 自适应开关，`desired` 期望槽，
`tailm` 抖动项（槽），`p99` 尾部 P99 (ms)，`tsamp` 窗内样本数（<128 则 P99 无效），
`min`/`max` 最小/最大槽 (ms)，`geo` 几何地板槽 (ms)，`src` margin 来源
（`tail_p99`/`stall_peak`），`path` 目标路径（`steady`/`rise`/`fall`/
`dwell_lock`/`storm_hold`/`deadband`/`no_time_base`），`fl_b`/`cap_b` 地板/上限夹住，
`pen` 欠载惩罚，`dwell` 剩余停留 ms，`fall` 下落余量槽，
`stall`/`peak`/`lastgap`/`arr` stall 事件/峰值/最近间隔/到达间隔，
`bw_lo`/`bn_lo`/`bn_hi`/`bw_hi` 水位带（槽），`crun`/`urun` 掩盖/欠载连跑。
第二轮（2026-10-19）：`eff_min` **真正生效的下限** = min_target + trunc(欠载惩罚)
（没有它，"target=7" 无法拆解成"配置写 3 + 惩罚 4"），`storm` 当前是否处于风暴
保持态（逐拍量，1Hz 采样撞不上 `path=storm_hold`），`stall_m` 峰值项本轮给了几槽
（`src` 只说 margin 谁赢，输的那一项的量级此前不可见），`stall_cap` 峰值项上限，
`fall_rate` 跌侧限速速率（解释"这一拍为什么只降一格"）。

**pb**（播放）：`running` 是否运行，`pstate` 播放状态，`err` 最近音频错误，
`pull`/`pf`/`sil` 拉取次数/帧/静音帧。
第二轮（2026-10-19）：`stale` 播放事件因过期被丢弃的次数（事件归属契约的
**唯一可观测量**——文档钦定，此前两个快照里都没有），`empty_skip` WASAPI 侧
"事件到了但端点缓冲**已满**、无事可写而跳过渲染"的次数。
⚠️ **它不是 underrun**：缓冲满 ⇒ 设备还没消费完 ⇒ 我们跑在设备**前面**（输出
延迟顶在上限）。真正的"**我们迟到了**"信号是 `catchup`/`catchup_max` 偏大
（设备已把缓冲吃空，我们必须一次补一大块；正常 ≈ 引擎周期，远大于周期即迟到）。
AAudio 侧的 `xrun`（`xrun_count`）才是真正的欠载计数 —— **两边语义不同，
不可互换**。
`cb_frames` 最近一次设备回调请求的帧数（几何地板的权威输入），`rate_adj`
系统 SRC 是否改过采样率。

**stream**（音频流）：`backend` 后端，`rate` 采样率，`ch` 通道数，
`perf` 性能模式，`fpb` 每 burst 帧，`cap` 缓冲容量帧，`cb` 回调次数，
`pad` 当前 padding 帧，`xrun` xrun 次数。
第二轮（2026-10-19）：`ep` **选定**引擎周期，`ep_min`/`ep_max`/`ep_def`/`ep_fund`
引擎周期的四个界（`ep_fund` 是 `ep` 的"粒度单位"——选定周期必是它的整数倍，
两个都报才解释得了"周期为什么是 336 而最小是 48"）。
**注意**：`fpb` 自 2026-10-19 起在 WASAPI 侧报的是**选定周期**而非历史上的
fundamental（后者可能小好几倍，几何地板因此算错——已修）。
`resample` 端点混音格式的**采样率或声道数**与请求格式不同 ⇒ 引擎要做重采样/混音
转换（Windows 上音频发飘/发晚的头号原因，此前只有一行启动日志），`mix_rate`/`mix_ch`
端点混音格式。
`buf_size` 后端**实际**缓冲帧数（WASAPI = 端点缓冲；AAudio = `getBufferSizeInFrames`
——**真正决定 AAudio 输出延迟的是它，不是 capacity**），`out_buf` **此刻**压在输出
设备缓冲里的帧数（WASAPI = GetCurrentPadding；AAudio = 已写入帧数 − 设备已播位置）。
两者同在"采样帧"这条时间轴上，除以采样率即毫秒 —— 这是**输出侧延迟的地板**。
远端协商格式（服务端给的原样，与本机播放流的实际参数对照）：`rmt_rate`/`rmt_ch`/
`rmt_enc`/`rmt_kbps`/`rmt_F`。

## 8. 架构：快照 → SnapshotView → SnapshotLine 三层

```text
采集线程/网络线程  ──写入──>  Snapshot (POD 值语义聚合)
                                        │  take_diagnostics_snapshot() 每秒刷一次
                                        ▼
                              SnapshotLine (加 module{...} 外壳, 每秒一行)
                                        │  add_source("net", [&]{ return snapshot_view.render_net(*s); })
                                        ▼
                     SnapshotView (每个模块一个 render_* 方法, 持持久 RateCounter)
                                        │  b.field(...).rate(...)
                                        ▼
                        FieldBlock + RateCounter (字符串拼接底层)
```

- **Snapshot**（`server_diagnostics_snapshot.h` / `client_diagnostics_snapshot.h`）：值语义 POD 聚合，是「采哪些量」的唯一真相源，被
  CLI 日志 **和** C API（Android/JNI）共用。读取是 relaxed atomic 的近似读，不保证一拍内完全自洽——但 diag tick 会先
  整体刷新一次快照，保证同一行内各块来自同一份近似读值。
- **SnapshotView**（`snapshot_view.h` / `snapshot_view.cpp`）：每个模块一组**持久的 `RateCounter`**，这就是该模块的「诊断 State 结构」。
  跨拍的 delta/rate 状态只活在 SnapshotView 实例里，不参与快照。快照负责「采什么」，SnapshotView 负责「怎么显示」，二者解耦。
- **FieldBlock / RateCounter**（`field_block.h` / `field_block.cpp`）：底层 `k=v` 拼接与
  `T/D/R` 速率格式化助手。`FieldBlock::field()` 用**模板**覆盖全部整型宽度、`enum class`
  与浮点（浮点默认 2 位小数、可显式指定精度），另有 `const char*` 精确匹配重载
  （否则字符串会掉进 `bool` 重载打成 `true`）；`bool` 打印 `true/false`，
  `std::string_view` 原样输出。

> 想新增一个模块的诊断：在快照里加字段 → 在对应 `SnapshotView::render_*` 里渲染 →
> 在 `server_main` / `client_main` 里用 `line.add_source("模块名", ...)` 注册。

## 9. 真实样例

下面两行来自一次本地回环 run，展示各块拼在一起的样子（列随版本增减，
以 `render_*` 实现和 第7节 为准，不要逐字对照旧快照）：

```text
Server diag: state{state=true sess=0 udp=50000} audio{capture=true ...} capture{ev=0/0/0.0 pq=440/40/32.4 ...} pktz{...} queue{depth=3 hwm=16 ...} dsp{...} net{served=v4+v6 rx=0/0/0.0 ... v4rx=0/0/0.0 v4tx=0/0/0.0 v6rx=0/0/0.0 v6tx=0/0/0.0} sess{act=0 ...}
```

```text
Client diag: state{state=true route=follow_system ...} net{rx=685/343/275.8 ... jit=0.40 ...} jb{water=0.10 ... fill_ep=0/0/0.0 ... drop_ep=1/0/0.0 ...} jc{adaptive=true desired=4 ...} pb{running=true ...} stream{backend=wasapi ... xrun=0/0/0.0}
```

