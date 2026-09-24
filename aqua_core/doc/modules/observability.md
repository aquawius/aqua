# 模块：Logger / Diagnostics / 日志点位

本文是 **日志点位的唯一索引**：看到某行日志该去哪查、开哪个开关、动哪个旋钮。诊断口径与字段语义见
`../diagnostics.md`；数值来源见 `../configuration_reference.md`。

## Logger

统一经 spdlog。sink 按平台选择：Windows / Linux 用 `stdout_color_sink_mt`；Android 用 `android_sink_mt`，tag 为 `aqua`
（Android 上 native stdout 不是可靠的日志输出）。

级别：`Trace` / `Debug` / `Info` / `Warn` / `Error` / `Fatal`，默认 `Info`。解析时接受别名（`warning` / `critical`）。

级别设置途径：

- CLI：`--log-level <name>`（启动时一次性设定）；
- C API：`aqua_client_config` 的 `log_level` 字段，以及 `aqua_set_log_level()`。

### 日志 tee（`--log-file <path>`）

两个 CLI 都支持把日志同时写到文件（spdlog 默认 logger 挂 `dist_sink`：控制台 + 文件）。

- **截断模式**：一次运行一个文件（启动即 truncate），便于按 run 分析。
- **文件与控制台同一条流**：级别仍只由 `--log-level` 决定，不做 per-sink 级别——
  想"安静控制台 + 完整文件"就把 stdout 重定向掉（例如 `> NUL`）。
- **每条日志即刻落盘**：挂了文件 sink 时 logger 用 `flush_on(trace)`，因此
  **内容不依赖优雅退出**。现场常用 Ctrl+C / taskkill 硬杀，那条路径不会走
  `shutdown_logger()`；若依赖缓冲，低音量运行（server 启动十几行 < 4KB）会整段丢在
  libc 缓冲里。该契约由 `tests/logger/logger_test.cpp` 的 `LogTeeTest` 钉住
  （测试刻意不调用 `shutdown_logger()`）。
- **文件打不开不中断启动**：退回纯控制台并打一条 warn（日志系统本身不能拖垮启动）。
- Android 侧同源实现（`basic_file_sink` / `dist_sink` 与平台无关，include 必须放在
  平台分支之外）。

**没有环境变量入口**，也没有运行时命令：CLI 启动后不能再改。日志文本统一为 UTF-8；Windows 上的 system error 按 `error_code`
的 category 分派渲染：`generic_category`（errno 语义，含 `std::thread` 抛的 `std::system_error`） 取 CRT errno 文本（ASCII）；
**其余类别**（`system_category` 与 asio 自有的 `"asio.system"`——它的值同样是 WSA/GetLastError 码）统一 `FormatMessageW` +
UTF-8 转换，失败才退化成 code-only。刻意不用 `ec.message()`
渲染 asio 错误：它内部走 `FormatMessageA`，返回 ACP 窄字符，在 UTF-8 日志里是乱码。

只调级别 **看不到**下面两个宏门控的点位——它们是编译期开关，需要重新构建。

## 日志的线程模型（为什么是同步的）

本项目踩过一次很贵的坑： **每秒一次的诊断行（~2.5KB 格式化 + 同步写控制台）跑在网络 io_context 上**， 每次阻塞数毫秒到数十毫秒，于是在
UDP 收发路径上制造出 **1Hz、18~20ms 的包到达空隙**， 接收端把它当成网络抖动、把自适应 target 顶高——测量装置污染了测量对象（同一份代码在
不开 debug 时完全没有这个现象）。

当时解法是异步 logger；后来发现异步的代价更大：关机尾部日志断行/丢失（async 队列 + thread_pool 静态析构时序竞争），且排查全靠尾部日志。
重新审计确认热路径干净后切回同步，现在有三条硬规则：

1. **默认 logger 是同步 `spdlog::logger`**：调用返回前即写完，天然全序、零丢失、
   任何退出路径不断行。同步写曾经的代价已不存在——诊断行在独立线程，
   热路径（网络 strand / 音频 RT / dispatcher）上没有无门控的逐包/逐回调日志。
2. **1s 诊断快照（snapshot + 打印）跑在独立 `std::jthread` 上**（CLI 的 server / client 两端皆然）， 不再占用 io_context；诊断
   getter 本身是线程安全的快照读取（C API 契约本就允许任意线程调用）。
3. **进程正常退出时显式 `shutdown_logger()` 排空**（CLI 的 `LogDrain` 在 worker join
   之后调用；不调用 `spdlog::shutdown()`：它会把 default logger 置空，而 spdlog 的 `log()`
   是无保护的 `default_logger_raw()->log(...)`，更晚的静态析构里 再打日志就是空指针解引用——`log_*` 均已判空保命）。

两条遗留注意点：

- **日志参数在调用点求值**（spdlog 只惰性格式化格式串）。因此重复路径上带分配的参数 （如 `sender.address().to_string()`）要用
  `if (aqua::log_level_enabled(...))` 包起来， 否则级别关闭时也在分配。
- 同步写下没有队列：调用返回即落盘，不存在"队列满丢日志"；进程被强杀最多丢正写到一半的一行。
## Diagnostics

`SnapshotLine`（`aqua/diagnostics/snapshot_line.h`，**CLI-only**）不拥有 runtime state，只注册 getter 并在快照时读取：

- `add_source(name, fn)`：注册一个模块块，fn 返回块的**内部**内容（由 `SnapshotView` 渲染）；
- 块内的 `T/D/R` 由 `RateCounter`（`field_block.h`）产出，rate 用真实 elapsed 计算，不假定定时器精确。

Debug 未启用时 `log_debug()` 直接返回，连 source 都不会调用——诊断 getter 跨多个 atomic 读取，不看 debug 日志就不该付这份
成本。

CLI 以 1s 周期打印；Android App 以 1s 节流（500ms 轮询 + 每两次取一次诊断）。

## 三个调试日志宏

音频链路上的日志点位分三类，分别由三个 **默认关闭**的编译期开关门控。彼此正交，可以只开一个。
划分轴是"哪条线程 + 哪一侧"：server/client 各有一条实时链，JB target 控制环只有 client 侧有。

| 宏                                              | 覆盖范围                                                                                                                                    | 线程                   | 默认 |
|-------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------|------------------------|------|
| `AQUA_SERVER_RT_DEBUG_LOG`                      | server 音频实时链：采集回调线程（含 `on_capture_block`）、packetizer 切包、paced dispatcher worker、broadcast；WASAPI 采集后端 RT 行       | 实时线程               | OFF  |
| `AQUA_CLIENT_RT_DEBUG_LOG`                      | client 音频实时链：JitterBuffer `pull()` / `decide()` / `apply_reanchor()`（含稳态 tick）、播放回调（含 `pull_playback` 外壳）；WASAPI/AAudio 播放后端 RT 行 | 实时线程               | OFF  |
| `AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG`       | 决策/判定层：`JitterEstimator::observe()`、`TargetController::update()`、`ClientRuntime` 的 push strand 回调、`JitterBuffer::push()`        | push strand / 控制线程 | OFF  |

`CMakePresets.json`：4 个 debug preset 三个都 ON，4 个 release preset 三个都 OFF。单独构建某个目标时， 各 `.cpp` 顶部有
`#ifndef ... #define ... 0` 兜底，不会因为漏传定义而编译失败。

### 为什么必须按线程分

实时线程上的日志会同步取 spdlog 的锁， **开启即破坏 RT 契约**（可能造成音频欠载）。控制面日志没有这个约束——push strand
上打日志只影响吞吐，不影响音频。把它们合成一个宏就只能在"牺牲 RT"和"失去排查能力"之间二选一。
server/client 再各拿一个 RT 宏：两端的实时链独立开关，查 server 发不出包时不用把 client 的渲染线程也拖下水。

### 门内门外的边界

- **门内**：音频线程的进入/退出标记 + 流运行期间的循环体。进入/退出标记成对，用来在日志里框出 RT 线程的生命周期。
- **门外（有意为之）**：
    - 音频线程在 **流建立之前**的设备打开 / 格式协商失败日志。此时流尚未运行、`start()` 仍阻塞在 `start_state` 上，不存在
      音频故障风险，而它们正是"设备打不开"的唯一诊断；
    - AAudio 的 `on_error_callback`：按 AAudio 语义它由独立的错误回调线程投递，不是 data callback 线程；
    - 启动期一次性诊断（`CLI config` / `CLI effective JB options` / `adaptive target controller` banner /
      `geometric floor`
      recalibrated）。理由见下一节。

### 为什么启动期诊断不设门

`CLI effective JB options` 这一行是 Release 现场（两个宏都关）排查"**参数到底生效没有**"的唯一入口。把它挂到控制面门
下就等于在最需要它的构建里把它关掉，因此它只受 `--log-level` 控制。

## 点位全表

### A. client RT 门内（`AQUA_CLIENT_RT_DEBUG_LOG`）

| 日志前缀                                                                                       | 位置                                             | 何时出现                         | 排查用途                                                                                        |
|------------------------------------------------------------------------------------------------|--------------------------------------------------|----------------------------------|-------------------------------------------------------------------------------------------------|
| `JitterBuffer timeline anchor established:`                                                    | `pull()`                                         | 启动 pre-roll 达标、建立播放锚点 | 启动→稳态的切换点（RT 侧视角）；`startup_level(startup)={}` 是生效的锚定水位（槽）              |
| `JitterBuffer water adjustment: FILL enter/complete/continue`                                  | `decide()`                                       | 低水位进入填充 episode           | 慢放修正在何时开始、每次走几步（`episode_steps` 大 = 还债风暴）                                 |
| `JitterBuffer water adjustment: DROP enter/complete/applied/deadline-high`                     | `decide()` / `pull()`                            | 高水位跳槽                       | `drop_duty` 偏高时定位到具体是哪一种 DROP（`warning=` 递增 = 持续偏移，`deadline-high` = 兜底） |
| `JitterBuffer water adjustment: reanchor request received=`                                    | `pull()`                                         | producer 投递了 reanchor 请求    | 请求与应用的配对起点                                                                            |
| `JitterBuffer water adjustment: REANCHOR play a->b highest c->d used e->f removed_ready=N`     | `apply_reanchor()`                               | 请求被应用                       | 时间轴跳变的实际幅度（`play_seq` 前后、清掉多少 READY 槽）                                      |
| `JitterBuffer reanchor hold-stuck fallback:`                                                   | `pull()`                                         | 等 target 等不到，兜底放弃 Hold  | **reanchor 后永久静音**类问题的第一现场：`pulls` 是否顶到阈值、`lead` 是否卡住不动              |
| `JitterBuffer underrun enter (no real PCM):`                                                   | `mark_underrun()`                                | 进入"无真实 PCM"的那个瞬间       | 之前只有 `underrun_events` 计数器；这一行给出当时的水位/target/掩盖状态                         |
| `JitterBuffer conceal enter (repeat last PCM):`                                                | `on_slot_boundary()`                             | 一次掩盖 episode 的第一个包      | "刚才那段是掩盖音还是真音"；`gain_q15` 是本次淡出增益                                           |
| `JitterBuffer conceal saturated -> silence:`                                                   | `on_slot_boundary()`                             | 掩盖达到上限、转静音的那一步     | 掩盖变静音的位置（听感上从"重复音"变成"断掉"）                                                  |
| `JitterBuffer conceal exit (real PCM back):`                                                   | `on_slot_boundary()`                             | 真实数据回来、episode 结束       | 单次 episode 的完整长度（`run_slots`）与累计掩盖量                                              |
| `WASAPI playback stop event received` / `WASAPI playback audio thread exited`                  | 渲染线程                                         | 线程进出标记                     | 框出 RT 线程生命周期，确认渲染线程是否真的退出（静默死流排查）                                  |
| `WASAPI playback callback exception:` / `returned N frames, but only M are available`          | 渲染线程                                         | 回调抛异常 / 契约被违反          | 播放破音的 RT 侧第一现场（同一错误也会经错误事件线程上报）                                      |
| `ClientRT pull:`                                                                               | `pull_playback()`                                | 每次播放回调                     | 消费侧节拍：请求/吐出/静音 + 当时水位；和到达侧 JBT 对上看收发拍频                             |
| `JBP act= lead= target= used= play= highest= filled= sil= conc= skipped=` | `pull()`                          | 每次播放回调（约 100Hz）   | **消费侧稳态节拍**：这一拍播走了什么（真实/静音/掩盖各多少）、水位与目标差多少。和到达侧 `JBT` 并排看可读出采集 10ms 与渲染 10.667ms 的拍频（启动 DROP 风暴的直接证据） |
| `AAudio playback data callback exception` / `callback returned N frames, but only M requested` | data callback                                    | 回调抛异常 / 契约被违反          | 同 WASAPI 回放，AAudio 侧的等价点位                                                             |
| `AAudio playback: dispatching runtime error event:`                                            | `report_fatal_once()`（可由 data callback 调用） | 运行期致命错误投递               | 每条流至多一行（`fatal_reported_` 保证），定位错误进入恢复链的时刻                              |

### B. client JB target 控制面门内（`AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG`）

| 日志前缀                                                          | 位置                         | 何时出现                           | 排查用途                                                                                                                                                                            |
|-------------------------------------------------------------------|------------------------------|------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `TargetController change:`                                        | `TargetController::update()` | target 变化时**每次**              | 事件驱动全量：`desired` 与 `current` 的差、`margin` 两项各自值、`src`（`tail_p99`/`stall_peak`）、`floor_bind`/`cap_bind`、`penalty`、`path`、`fall_room`、`dwell_left`、`storm`、`tail_p99_ms`                   |
| `TargetController steady:`                                        | 同上                         | target 不变时，每 5s 一行          | **稳态下控制器在做什么**：`path=steady` = desired 真的等于 current；`path=dwell_lock`/`fall` = 被 dwell/限速按住；`path=storm_hold` = 风暴冻结中；`path=deadband` = 被死区吞掉。7↔8 抽动类问题就靠它区分            |
| `JitterEstimator stall:`                                          | `observe()`                  | 每次判定为 stall                   | `gap` / `threshold`（含包周期倍数）/ `peak before -> after` / 生效的 `decay`。排查"峰值为什么挂这么久"必看                                                                          |
| `JitterEstimator transit step:`                                   | `observe()`                  | 单包 \|Δtransit\| ≥ `JB_TRANSIT_STEP_PACKETS`（包单位） | 路径电平跳变的方向 + 幅度 + 电平前后值。stall 行回答"到达断了多久"，这行回答"路径跳了多少"；非 stall 台阶（路由/AP 切换）只在这里出现 |
| `JitterEstimator anchor established (first packet):`              | `observe()`                  | 每流首包                           | 观测窗口的起点（开局 ~60ms 收敛期的第一块拼图）                                                                                                                                     |
| `JitterEstimator J milestone:`                                    | `observe()`                  | 第 1/16/64/256 个样本              | `J` 从 0 到收敛的轨迹（含 `transit` / `base` / `interval` / `peak`）                                                                                                                |
| `JitterEstimator transit anchor rebuilt (sender timeline reset):` | `observe()`                  | timestamp 非单调                   | 发送端时间轴重置：本包不进 J，只重建锚点。排查"J 为什么突然归零/不涨"                                                                                                               |
| `JitterEstimator reset on SSRC change:`                           | `observe()`                  | SSRC 变化                          | 新流接管：旧时间轴全部作废（含重置前的 `packets` / `stalls` / `J` / `peak`）                                                                                                        |
| `JitterBuffer reanchor probe:` / `(pre-start)`                    | `JitterBuffer::push()`       | 远超前距离达到容量                 | **为什么 reanchor / 为什么没 reanchor**：`distance`、`gap_from_highest`、`min_gap`、`capacity`、结论。未达断裂缺口（环形满溢）时分母为 `JB_CONTROL_LOG_REANCHOR_PROBE_EVERY` 的节流 |
| `JitterBuffer reanchor sanity reject:` / `(pre-start)`            | `JitterBuffer::push()`       | 跨度超过 `max_span`                | 荒谬请求被拒的具体跨度（`span_frames` vs `max_span`）                                                                                                                               |
| `ClientRuntime push rejected (first):`                            | arrival observer             | 本会话**第一个**被拒的包           | 精确定位第一个被拒的包（`seq` / `play_seq` / `highest`），不必等 1s diag 行                                                                                                         |
| `ClientRuntime push rejected:`                                    | 同上                         | 之后每秒最多一行（有变化才打）     | 拒绝原因分布增量：`+late` / `+busy` / `+invalid` / `+sanity` 与累计值。`busy` 持续涨 = 结构性配置问题，见 `configuration_reference.md` 的 2/3 上限                                  |
| `JBT seq=...`                                                     | arrival observer             | 每包一行（仅 `--jb-trace` 开启时） | **trace 级**（默认 debug 视图不可见）：离线回放的输入；parser 只认 `seq/ts/arr_ns`，尾巴字段可增不可减。采语料用 `--log-level trace` + `--log-file` |
| `JBQ seq= used= play= highest= lead= target= cap=`                          | `JitterBuffer::push()` 入槽成功处 | 每包（约 274Hz）           | **生产者侧稳态节拍**：入槽后的水位/领先量/目标。与消费侧 `JBP` 对齐看进出是否平衡——`used` 涨而 `JBP` 的 `filled` 不变 = 消费侧卡住，不是网络问题 |

### C. server RT 门内（`AQUA_SERVER_RT_DEBUG_LOG`）

server 侧五问逐条对应：**产生了多大 → 切包剩多少 → 入队多少 → 本批发几包 → 发给谁**。
全部跑在采集回调线程或 paced dispatcher worker 上，因此一律走宏。

| 日志前缀                                                        | 位置                                   | 何时出现                 | 排查用途                                                                                      |
|-----------------------------------------------------------------|----------------------------------------|--------------------------|-----------------------------------------------------------------------------------------------|
| `ServerRT capture block: bytes= frames=`                        | `on_capture_block()` 入口              | 每个采集块（约 100Hz）   | 采集侧实际交付量；`frames=0` 说明格式几何（frame_bytes）算错                                  |
| `ServerRT packetized: cut= pending_leftover=`                    | 同上，packetizer 回调之后              | 每个采集块               | 本块切出几包、半包尾部剩几字节。`pending_leftover` 长期不归零 = 块长与包长不整除以致持续欠一包 |
| `ServerRT enqueued: seq= bytes= queue_accepted= queue_depth=`    | packetizer sink 回调                   | 每包                     | 入队是否被接受。`queue_accepted=0` = 交接队列满（下游发不动），`queue_depth` 给出积压         |
| `ServerRT publish: notify= queue_depth=`                         | `publish_from_realtime()`              | 每帧                     | 采集线程 → paced worker 的交接。只有 `sent` 行却没有 `publish` 行 = 采集侧根本没交付          |
| `ServerRT paced batch: sent= queue_left=`                        | `drain_paced()`                        | 本 tick 真发了包才打     | 一次 pacing tick 补发几包、还剩几包（空转 tick 静默）。`sent` 长期 >1 = 正在追赶              |
| `ServerRT catchup send: queue_left=`                             | `drain_paced()` catchup 分支           | 积压超过 catchup 门限    | 进入追赶模式的时刻与当时积压深度                                                              |
| `ServerRT sent: seq= recipients= queue_left=`                    | `send_one()`                           | 每包                     | **发给谁**：`recipients` = 本包送达的 session 数（`-1` = broadcast 抛异常）                    |
| `UdpServer broadcast recipients changed: n= [...]`               | `UdpServer::broadcast()`               | 接收端集合变化才打       | 首个 client 接入 / 成员增减 / NAT 漫游的时刻与完整端点列表。逐包不打（274Hz×N 没人看）        |
| `WASAPI(capture): AvSetMmThreadCharacteristicsW ... failed`      | 采集线程启动期                         | MMCSS 注册失败           | 采集线程没拿到 Pro Audio 优先级（线程期一次性；归宏是因为它运行在 RT 线程上）                 |
| `ServerRT capture block dropped: state= bytes=` | `on_capture_block()` 静默丢弃分支 | 停止/切换窗口期间采集仍在交付但被丢弃 | 这段"采到了但没进网络"此前不可见；state 非 Running 或空块时触发 |
| `ServerRT handoff queue overflow (...)` | packetizer sink 回调 | 交接队列满（`queue_accepted=0` 时） | 与 `ServerRT enqueued` 的 `queue_accepted=0` 同位置，首条即时 + 按秒汇总（含 depth/capacity/dropped） |
| `ServerRT packetizer rejected unaligned block (...)` | `on_capture_block()` 包化后 | 未对齐块被整块丢弃（设备/格式几何配错） | packetizer 自身静默，只有 `unal` 计数器；首条即时 + 按秒汇总 |
### D. 不设门的运维日志（只看 `--log-level`）

| 日志前缀                                                                            | 何时出现                       | 备注                                                                                                 |
|-------------------------------------------------------------------------------------|--------------------------------|------------------------------------------------------------------------------------------------------|
| `CLI config:`                                                                       | 启动期                         | 完整生效配置（含 server / 播放设备 / 心跳）                                                          |
| `CLI effective JB options (src: cli=explicit, default=built-in):`                   | 启动期                         | 6 个 `--jb-*` 数值旋钮的最终值与**来源**，一行回答"用户到底传没传参"                                 |
| `ClientRuntime adaptive target controller:`                                         | 启动期                         | controller 的全部生效参数（含 `stall_cap` / `stall_decay` / `stall_threshold`，0 值会如实显示）      |
| `JitterBuffer created:`                                                             | 启动期                         | JB 几何与固定模式水位带                                                                              |
| `ClientRuntime adaptive target:`                                                    | target 变化时                  | **既有的** target 变化行（水位带、`src`、夹持状态）。控制面门关闭时它仍然在，是 Release 下的主要抓手 |
| `ClientRuntime network stall:`                                                      | 每次 stall                     | **既有的** stall 行。`stall_peak_ms` 与 `- not into J, handled ...` 说明它没进 J                     |
| `ClientRuntime startup anchored (startup -> steady):`                               | 每会话一次                     | 启动 pre-roll 达标、播放锚定建立；给出锚定瞬间的水位/target/used（此前在控制面门内，现已解门）       |
| `ClientRuntime storm entered/exited:`                                               | 风暴进出边沿                   | `path=StormHold` 只在冻结拍出现，target 不动时风暴隐形——这行补上进出的时刻（含当时 target/desired）  |
| `ClientRuntime reanchor applied:`                                                   | 重锚定应用（warn 级）          | 每次应用都扔一批已缓存（可闻断裂）：首次即时，风暴期按秒汇总次数 + 当时 play/lead/target             |
| `ClientRuntime geometric floor recalibrated ...`                                    | 设备切换导致 callback 几何变化 | 几何地板被校正（`min_target` 前后值）                                                                |
| `ClientRuntime push rejected` 之外的 `push_rejected_*` 计数器                       | 1s diag 行                     | 稳态汇总口径，见 `../diagnostics.md`                                                                 |
| `WASAPI playback started:` / `WASAPI capture started:` / `AAudio playback started:` | 启动期                         | 后端实际协商结果                                                                                     |

## 排查指引

| 症状                            | 先看哪行                                                                                              | 再动哪个旋钮                                                                                                                                      |
|---------------------------------|-------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| `target` 钉在高位不下来         | `JitterEstimator stall:` 的 `peak`、`TargetController steady:` 的 `stall` 项与 `path`                 | `--jb-stall-decay`（调大 = 更快忘记）、`--jb-stall-peak-cap`（调小 = 最多推多高）                                                                 |
| `target` 在相邻两格之间反复横跳 | `TargetController change:` 的 `src` 与 `path`                                                         | 先确认是尾部摆动（`src=tail_p99`，看 `tail_p99_ms` 是否真在动）还是 `stall_peak` 摆动；尾部看窗口记忆（`tsamp` 是否刚过门限），stall 看 `--jb-stall-*`。`dwell_left` 非 0 说明锁跌正在生效，`storm=1` 说明风暴冻结中                    |
| `target` 卡在 desired−1 不动    | 不可能发生（死区参数已删除，恒 0；见 ADR-4）                                                         | 若出现，说明有人重新引入了死区——直接删掉，不要调                                                   |
| 持续欠载（`underrun_ratio` 高） | `JitterBuffer underrun enter` 的水位/target + `ClientRuntime network stall` + `JitterEstimator stall` | 先分清是"抖动超出预测"（看 `tailm`/`p99` 是否跟上）还是"stall 尾部被门剔除"（`--jb-stall-peak-cap`）；`--jb-underrun-penalty 0` 可确认反馈项是否在起作用 |
| `busy` 拒绝率异常高             | `ClientRuntime push rejected:` 的 `+busy` + 1s diag 的 `used/target`                                  | 结构性：`target` 顶穿 2/3 上限（`configuration_reference.md`）。先看 `margin` 是哪项推高的，不要动容量                                               |
| 播放头停住、声音全无            | `JitterBuffer reanchor hold-stuck fallback:`、`water adjustment: REANCHOR`                            | 看 `pulls` 是否顶到阈值、`lead` 是否卡住；`min_gap` 相关判定见 `JitterBuffer reanchor probe:`                                                     |
| 怀疑 reanchor 该触发却没触发    | `JitterBuffer reanchor probe:` 的结论字段                                                             | `no-gap: ring overflow` = 交由 `deadline-high DROP` 兜底，属预期                                                                                  |
| 阵发性卡顿（一阵一阵的）        | `storm entered/exited` 的进出时刻 + `reanchor applied` 的次数/水位                                    | 先分清性质：`gap>0` 是丢失型（调 margin/penalty），`gap=0` + `busy` 涨是回补型（加 `--jb-capacity`，见设计文档 §9 回补风暴节） |
| "刚才那段是掩盖音还是真音"      | `JitterBuffer conceal enter/saturated/exit`                                                           | `--jb-no-conceal` 做听感 A/B                                                                                                                      |
| 怀疑参数没生效                  | `CLI effective JB options` 的 `cli` / `default` 后缀                                                  | ——                                                                                                                                                |
| transit 台阶（路径疑似变了）      | 1s diag 的 `trlvl`（电平）/`tstep`（台阶计数）；`tstep` 涨但无 stall 行 = 纯路径切换（路由/AP），有 stall 行对照 = 大间隙的两面 | 只观测不进控制；先查 WLAN roam 日志 / AP / 路由，再考虑 `--jb-min-target` 预垫                                                                       |
| 参数传了但行为没变              | `ClientRuntime adaptive target controller:` banner 的 0 值                                            | `--jb-fixed-target` 打开时控制器根本不建，见 `configuration_reference.md`                                                                         |
| server 在跑但 client 一个包都收不到 | `ServerRT publish:` / `ServerRT sent:` 的 `recipients` + `UdpServer broadcast recipients changed` | `recipients=0` = 没有 Connected session（UDP 握手没到，看 server 侧 heartbeat 行）；`recipients>0` 但 client 侧没有 `UDP datagram received` = 路径问题（NAT / 防火墙 / 通告地址不对） |

## 日志节奏常量（不是调参项）

以下常量只影响日志密度，改它们不会影响音频行为，都定义在 `buffer_config.h`：

- `JB_CONTROL_LOG_SUMMARY_INTERVAL_MS`（5s）：`TargetController` 稳态摘要的节流周期；
- `JB_CONTROL_LOG_REJECT_INTERVAL_MS`（1s）：push 拒绝汇总的节流周期（首次不节流）；
- `JB_CONTROL_LOG_REANCHOR_PROBE_EVERY`（128）：reanchor 探测日志在"未达断裂缺口"分支的节流分母。

## 使用约束

三个调试宏都 **只能短时间复现问题时开启**，不得作为生产基线：RT 宏开启后直接违反实时契约；控制面宏会把 push strand
的每包处理路径变成带锁的字符串格式化。排查完请回到 release preset（三个宏都 OFF）。
