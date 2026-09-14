# aqua 诊断输出（diagnostics）阅读指南

本文档说明 aqua server / client 每秒打印的那一行诊断信息（log 级别 `debug` 或
`trace` 才输出）该怎么读。它是在「每个计数器各自展开成 `total/delta/rate`
三个长字段、一行 2.5KB、基本不可读」的基础上做的一次大优化后的新格式。

> 这一行只在 **log level = debug / trace** 时打印。生产/Release 现场若只看
> info 及以上，看不到它——但 Android 侧的 JNI 诊断（读同一份快照）始终可用。

---

## 1. 总览：一行 = 若干「模块块」

每个诊断量按 **模块**聚合成一段段紧凑块，外层由 `Diagnostics` 包成 `module{...}`。 一行大致长这样（已折行以便阅读，实际是一行）：

```
Server diag: state{...} audio{...} capture{...} pktz{...} queue{...} dsp{...} net{...} sess{...}
Client diag: state{...} net{...} jb{...} jc{...} pb{...} stream{...}
```

- 块名（`state` / `audio` / `net` / `jb` / `jc` / `pb` / `stream` …）对应一个运行模块。
- 块内是 `k=v` 以空格分隔的键值对，例如 `depth=3 hwm=16`。
- 每个模块块由对应的 `DiagView` 渲染方法产出（见第 4 节架构）。

---

## 2. 速率字段的 `T/D/R` 简写

凡是「累计计数器」类型的字段（包/帧/事件计数等），值都渲染成 **`T/D/R`** 三段：

| 段  | 含义      | 说明                                         |
|-----|-----------|----------------------------------------------|
| `T` | **Total** | 进程启动以来的累计值                         |
| `D` | **Delta** | 距上一次诊断 tick（约 1 秒）的增量           |
| `R` | **Rate**  | 每秒速率 = `D / 实际经过秒数`，保留 1 位小数 |

示例：`rx=685/343/275.8` → 累计收到 685 个包，上一秒新增 343 个，速率约 275.8 包/秒。

边界约定：

- **首拍**没有基线，显示 `T/0/0.0`（delta、rate 记为 0）。
- 计数器 **回退**（当前 total 小于上次，极少见）时 `D` 记 0，避免出现负速率。
- `R` 用真实 `steady_clock` 间隔计算，不假定定时器精确为 1.000s。

纯 **仪表**（gauge）字段（水位、队列深度、延迟毫秒、枚举名等）直接显示值，没有 `/`。 部分仪表带单位或精度后缀，例如 `age=232ms`、
`tgt=4(14.6ms)`、`uratio=0.000045`、
`fduty=0.000000`。

---

## 3. 缩写对照表

块内键名走「短而稳定」路线。下面是各模块字段的速查表。

### Server

**state**
| 键 | 含义 | |----|------| | state | 运行时状态（Created/Starting/Running/Stopping/Stopped/Degraded…） | | sess | 当前活跃
session 数 | | udp | UDP 数据面端口 |

**audio**（采集链路总体） | 键 | 含义 | |----|------| | capture | 采集是否运行中 | | err | 最近一次音频错误（None
表示无） | | fmt | 音频格式 `Chch/SRk/encN`（如 `2ch/48k/enc3`，Hz 整除 1000 简写为 k） | | F | 每包帧数（packet frames） | |
src | 采集源（0=input 设备，1=loopback 系统混音） | | cstate | 采集状态枚举名 | | sw | 采集切换状态 | | route |
采集路由模式 | | dev | 指定设备 ID（仅当 route=preferred 且指定了设备时出现） | | ls | 最近一次切换结果/耗时
`outcome/ms` | | cf / cb | 累计采集帧数 / 字节数 | | pkt | 每包帧数 最近/最小/最大 `last/min/max` | | stv | 当前/最大
starved 毫秒 `cur/max` |

**capture**（采集回调层，`T/D/R` 速率）
`ev`=音频事件 `pq`=packet 查询 `pe`=空 packet `pr`=就绪 packet
`gb`=取缓冲成功 `cbk`=回调次数 `scb`=静音回调 `ssb`=合成静音块
`gsf`=生成静音帧 `se`=starved 事件 `stv_ms`=starved 毫秒
`cf`=采集帧 `cby`=采集字节

**pktz**（打包器，`T/D/R` 速率）
`blk`=输入块 `by`=输入字节 `frm`=发出帧 `unal`=拒绝的对齐错误块 `disc`=待丢弃

**queue**（采集→网络之间的缓冲）
`depth`=当前深度（槽） `hwm`=高水位（槽）
`acc`/`con`/`drop`=接受/消费/丢弃帧数（`T/D/R` 速率）

**dsp**（网络分发器，`T/D/R` 速率）
`enc`=编码帧 `bc`=广播帧 `nocl`=无客户端帧 `ef`=编码失败
`df`=分发失败 `drop`=丢弃帧 `pub`=发布帧 `wake`=worker 唤醒

**net**（传输层，`T/D/R` 速率 + 若干仪表）
`rx`/`tx`=收/发包 `rxB`/`txB`=收/发字节 `rxerr`/`txerr`=收发错误
`drop`=发送丢弃 `enqf`=发送入队失败 `q`=发送队列深度（仪表）
`hb_recv`=收到心跳 `hb_rej`=拒绝心跳 `sess_est`=建立 session
`sess_ref`=刷新 session `hb_ack`=心跳 ack 尝试 `mal`=畸形包 `nonhb`=非心跳包

**sess**（session 管理，`T/D/R` 速率）
`act`=活跃数（仪表） `crt`=创建 `con`=已连接 `ref`=刷新 `rem`=移除
`exp`=过期 `rmbc`=被清空移除

### Client

**state**
| 键 | 含义 | |----|------| | state | 运行时状态 | | route | 播放路由模式（follow_system / preferred / …） | | ls |
最近一次切换结果/耗时 `outcome/ms` | | seq | 切换序号（每次切换事务 +1，比 outcome 更能区分「又切了一次」） |

**net**（`T/D/R` 速率 + 仪表，同 server 的命名含义）
`rx`/`tx`/`rxB`/`txB`/`rxerr`/`txerr`/`drop`/`enqf`/`q`
`ack`=心跳 ack `miss`=ack 丢失数（仪表） `age`=最近 ack 年龄（仪表，`ms`）
`hb_fail`=心跳失败（仪表） `hshk`=握手发送尝试 `miss_ev`=ack 丢失事件
`af`=音频帧接受 `gap`=音频序列缺口事件 `gap_fr`=缺失帧
`mal`=畸形 `unsnd`=Unexpected Sender `wsa`=Wrong Session Ack `plm`=payload 不匹配 `nona`=非音频包
`jit`=抖动估计 (ms) `base`=基础延迟 (ms) `tr`=中转延迟 (ms)
`reord`=重排包 `dup`=重复包 `late`=迟到包

**jb**（客户端抖动缓冲，`T/D/R` 速率 + 仪表）
`water`=水位 (0~1) `used`=已用/容量槽 `lead`=lead 槽 (ms) `tgt`=目标槽 (ms)
`play`=播放序列号 `high`=最高收到序列号
`reanc`=重锚定次数 `req`=重锚定请求 `canc`=取消 `san`=合理性拒绝
`pend`=重锚定挂起 `tgtseq`=重锚定目标序列 `csil`=连续静音帧 `msil`=最大静音连跑
`ep`=当前情节（none / filling / dropping）
`push_ok`/`push_rej`=入缓冲接受/拒绝 `late`=迟到的拒绝 `busy`=槽忙拒绝 `inv`=非法拒绝 `sanity`=合理性拒绝
`pull`=拉取次数 `pf`=拉取帧 `sil`=静音帧
`fill_ep`=填充情节 `fill_sl`=填充修正槽 `drop_ep`=丢弃情节 `skip`=跳过槽
`und_ev`=欠载事件 `und_fr`=欠载帧 `uratio`=欠载比例 `max_und`=最大连续欠载槽
`conceal`= conceal 槽 `csat`=conceal 饱和 `fu`=迟到有用包
`fduty`=填充占空比 `dduty`=丢弃占空比

**jc**（抖动控制层，全部仪表）
`adaptive`=自适应开启 `desired`=期望槽 `min`=最小槽 (ms) `max`=最大槽 `geo`=几何地板槽 (ms)
`src`=margin 来源 `path`=目标路径 `fl_b`=被地板夹住 `cap_b`=被上限夹住
`pen`=欠载惩罚 `dwell`=剩余停留 ms `fall`=下落余量槽
`stall`=stall 事件 `peak`=stall 峰值 ms `lastgap`=最近 stall 间隔 ms `arr`=到达间隔 ms
`bw_lo`/`bn_lo`/`bn_hi`/`bw_hi`=静音/正常/警告带（槽） `crun`=conceal 连跑 `urun`=欠载连跑

**pb**（播放，仪表 + `T/D/R` 速率）
`running`=播放运行中 `pstate`=播放状态 `err`=最近音频错误
`pull`=拉取次数 `pf`=拉取帧 `sil`=静音帧

**stream**（音频流，仪表 + `T/D/R` 速率）
`backend`=后端（wasapi / …） `rate`=采样率 `ch`=通道数 `perf`=性能模式
`fpb`=每 burst 帧 `cap`=缓冲容量帧 `cb`=回调次数 `pad`=当前 padding 帧
`xrun`=xrun 次数（`T/D/R` 速率）

---

## 4. 架构：快照 / DiagView / Block 三层

```
采集线程/网络线程  ──写入──>  Snapshot (POD 值语义聚合)
                                       │  take_diagnostics_snapshot() 每秒刷一次
                                       ▼
                              Diagnostics (加 module{...} 外壳, 每秒一行)
                                       │  add_source("net", [&]{ return diag_view.render_net(*s); })
                                       ▼
                       DiagView (每个模块一个 render_* 方法, 持持久 RateCounter)
                                       │  b.field(...).rate(...)
                                       ▼
                          Block + RateCounter (字符串拼接底层)
```

- **Snapshot**（`server_diagnostics_snapshot.h` / `client_diagnostics_snapshot.h`）： 值语义 POD 聚合，是「采哪些量」的唯一真相源。被
  CLI 日志 **和** C API（Android/JNI） 共用。读取是 relaxed atomic 的近似读，不保证一拍内完全自洽——但 diag tick 会先
  整体刷新一次快照，保证同一行内各块来自同一份近似读值。
- **DiagView**（`diag_view.h` / `diag_view.cpp`）：每个模块一组 **持久的 `RateCounter`**， 这就是该模块的「诊断 State 结构」（类比
  `udp_server.h` / `udp_client.h` 把运行态收敛进 各自的 `State`）。跨拍的 delta/rate 状态只活在 DiagView 实例里，不参与快照。
  快照负责「采什么」，DiagView 负责「怎么显示」，二者解耦。
- **Block / RateCounter**（`diag_block.h` / `diag_block.cpp`）：底层 `k=v` 拼接与
  `T/D/R` 速率格式化助手。`Block::field()` 用 **模板**覆盖全部整型宽度、`enum class`
  与浮点（浮点默认 2 位小数、可显式指定精度），无需为每种类型补重载；`bool` 打印
  `true/false`，`std::string_view` 原样输出。

> 想新增一个模块的诊断：在快照里加字段 → 在对应 `DiagView::render_*` 里渲染 →
> 在 `server_main` / `client_main` 里用 `diag.add_source("模块名", ...)` 注册。
> 不必再手写 `add_counter`。

---

## 5. C API / Android 兼容性

快照结构体（`ServerDiagnosticsSnapshot` / `ClientDiagnosticsSnapshot`）的 **字段未改动**， 因此 `aqua_capi.h` 的
`aqua_client_diagnostics_t` 与 Android JNI （`nativeGetDiagnostics`，`kDiagnosticsCount=111` 的 LongArray）
**无需改动、继续可用**。 Android 侧仍按自己的逻辑从累计计数器算速率——渲染层（DiagView / Block）只服务于 CLI 日志，不影响 C
API 契约。

---

## 6. 真实样例（来自一次本地回环 run）

**Server**（loopback 采集、无客户端连接，部分计数器在涨是因为 capture 持续产出）：

```
Server diag: state{state=true sess=0 udp=50000} audio{capture=true err=true fmt=2ch/48k/enc3 F=175 src=1 cstate=true sw=true route=true ls=none/0ms cf=0 cb=0 pkt=0/0/0 stv=13528/0} capture{ev=0/0/0.0 pq=440/40/32.4 pe=440/40/32.4 pr=0/0/0.0 gb=0/0/0.0 cbk=0/0/0.0 scb=0/0/0.0 ssb=427/39/31.6 gsf=652682/59251/47996.4 se=1/0/0.0 stv_ms=0/0/0.0 cf=0/0/0.0 cby=0/0/0.0} pktz{blk=427/39/31.6 by=5221456/474008/383971.3 frm=3729/338/273.8 unal=0/0/0.0 disc=0/0/0.0} queue{depth=3 hwm=16 acc=3721/337/273.0 con=3718/337/273.0 drop=8/1/0.8} dsp{enc=3718/337/273.0 bc=0/0/0.0 nocl=3718/337/273.0 ef=0/0/0.0 df=0/0/0.0 drop=8/1/0.8 pub=3721/337/273.0 wake=407/38/30.8} net{rx=0/0/0.0 ... mal=0/0/0.0 nonhb=0/0/0.0} sess{act=0 crt=0/0/0.0 con=0/0/0.0 ref=0/0/0.0 rem=0/0/0.0 exp=0/0/0.0 rmbc=0/0/0.0}
```

**Client**（已连上 server、在放音，能看到 rx/推送/拉取都在涨）：

```
Client diag: state{state=true route=follow_system ls=none/0ms seq=0} net{rx=685/343/275.8 rxB=962999/482909/388266.2 rxerr=0/0/0.0 tx=3/1/0.8 txB=15/5/4.0 txerr=0/0/0.0 drop=0/0/0.0 enqf=0/0/0.0 q=0 ack=3/1/0.8 miss=0 age=464ms hb_fail=false hshk=1/0/0.0 miss_ev=0/0/0.0 af=682/342/275.0 gap=0/0/0.0 gap_fr=0/0/0.0 mal=0/0/0.0 unsnd=0/0/0.0 wsa=0/0/0.0 plm=0/0/0.0 nona=0/0/0.0 jit=0.40 base=-0.36 tr=0.02 reord=0 dup=0 late=0} jb{water=0.10 used=3/30 lead=3(10.9ms) tgt=4(14.6ms) play=14102 high=14104 reanc=0/0/0.0 req=0/0/0.0 canc=0/0/0.0 san=0/0/0.0 pend=false tgtseq=0 csil=0 msil=960 ep=true push_ok=682/342/275.0 push_rej=0/0/0.0 late=0/0/0.0 busy=0/0/0.0 inv=0/0/0.0 sanity=0/0/0.0 pull=249/124/99.7 pf=119520/59520/47856.2 sil=960/0/0.0 fill_ep=0/0/0.0 fill_sl=0/0/0.0 drop_ep=1/0/0.0 skip=3/0/0.0 und_ev=0/0/0.0 und_fr=0/0/0.0 uratio=0.000000 max_und=0 conceal=0/0/0.0 csat=0/0/0.0 fu=0/0/0.0 fduty=0.000000 dduty=0.004393} jc{adaptive=true desired=4 min=4(14.6ms) max=20 geo=3(10.9ms) src=true path=true fl_b=true cap_b=false pen=0.00 dwell=0 fall=0.00 stall=0 peak=0.0 lastgap=0.0 arr=3.993 bw_lo=1 bn_lo=2 bn_hi=5 bw_hi=6 crun=0 urun=0} pb{running=true pstate=true err=none pull=249/124/99.7 pf=119520/59520/47856.3 sil=960/0/0.0} stream{backend=wasapi rate=48000 ch=2 perf=low_latency fpb=1 cap=1056 cb=249 pad=576 xrun=0/0/0.0}
```

读这一行时的直觉顺序： **state**（整体活着吗）→ **net**（链路通不通、有没有丢/错）→ **jb**（缓冲水位/目标/重锚定/欠载，音质相关的第一现场）→
**jc**（目标为什么是这个值）→ **pb** / **stream**（播放侧是否真的在出声、有没有 xrun）。
