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

**没有环境变量入口**，也没有运行时命令：CLI 启动后不能再改。日志文本统一为 UTF-8；Windows 的 system error 经
`FormatMessageW` + UTF-8 转换归一化，避免 ACP 乱码。

只调级别 **看不到**下面两个宏门控的点位——它们是编译期开关，需要重新构建。

## 日志的线程模型（为什么必须是异步的）

本项目踩过一次很贵的坑： **每秒一次的诊断行（~2.5KB 格式化 + 同步写控制台）跑在网络 io_context 上**， 每次阻塞数毫秒到数十毫秒，于是在
UDP 收发路径上制造出 **1Hz、18~20ms 的包到达空隙**， 接收端把它当成网络抖动、把自适应 target 顶高——测量装置污染了测量对象（同一份代码在
不开 debug 时完全没有这个现象）。

因此现在有三条硬规则：

1. **默认 logger 是 `spdlog::async_logger`**：4096 条有界队列 + `overrun_oldest` 溢出策略。 调用线程只做 **级别判断 +
   格式化 + 入队**（时间戳在调用点取，所以行内时间仍然准确）， sink 写（console / logcat）在后台线程。
   **实时与网络路径上永远不会因为写日志而阻塞。**
2. **1s 诊断快照（snapshot + 打印）跑在独立 `std::thread` 上**（CLI 的 server / client 两端皆然）， 不再占用 io_context；诊断
   getter 本身是线程安全的快照读取（C API 契约本就允许任意线程调用）。
3. **进程正常退出时 `atexit` 里 `flush()`**（不调用 `spdlog::shutdown()`：它会把 default logger 置空，而 spdlog 的 `log()`
   是无保护的 `default_logger_raw()->log(...)`，更晚的静态析构里 再打日志就是空指针解引用）。

两条遗留注意点：

- **日志参数在调用点求值**（spdlog 只惰性格式化格式串）。因此重复路径上带分配的参数 （如 `sender.address().to_string()`）要用
  `if (aqua::log_level_enabled(...))` 包起来， 否则级别关闭时也在分配。
- 队列满时 **丢弃最旧的一条**（实时性优先于日志完整性）；进程被强杀会丢尾部日志。

## Diagnostics

`Diagnostics` 不拥有 runtime state，只注册 getter 并在快照时读取：

- `add_source(name, fn)`：返回一行字符串快照；
- `add_counter(name, fn)`：按快照间隔输出 `total` / `delta` / `rate`，rate 用真实 elapsed 计算，不假定定时器精确。

Debug 未启用时 `log_debug()` 直接返回，连 source 都不会调用——诊断 getter 跨多个 atomic 读取，不看 debug 日志就不该付这份
成本。

CLI 以 1s 周期打印；Android App 以 1s 节流（500ms 轮询 + 每两次取一次诊断）。

## 两个调试日志宏

JitterBuffer 链路上有两类日志点位，分别由两个 **默认关闭**的编译期开关门控。两者正交，可以只开一个。

| 宏                                 | 覆盖范围                                                                                                                                                         | 线程                   | 默认 |
|------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------|------|
| `AQUA_JB_RUNTIME_THREAD_DEBUG_LOG` | 会**在音频流运行期间**执行的日志：JitterBuffer `pull()` / `decide()` / `apply_reanchor()`，WASAPI 渲染线程与采集线程的循环体及线程进出标记，AAudio data callback | 实时线程               | OFF  |
| `AQUA_JB_CONTROL_THREAD_DEBUG_LOG` | 决策/判定层：`JitterEstimator::observe()`、`TargetController::update()`、`ClientRuntime` 的 push strand 回调、`JitterBuffer::push()`                             | push strand / 控制线程 | OFF  |

`CMakePresets.json`：4 个 debug preset 两个都 ON，4 个 release preset 两个都 OFF。单独构建某个目标时， 各 `.cpp` 顶部有
`#ifndef ... #define ... 0` 兜底，不会因为漏传定义而编译失败。

### 为什么必须分两个

实时线程上的日志会同步取 spdlog 的锁， **开启即破坏 RT 契约**（可能造成音频欠载）。控制面日志没有这个约束——push strand
上打日志只影响吞吐，不影响音频。把它们合成一个宏就只能在"牺牲 RT"和"失去排查能力"之间二选一。

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

### A. RT 门内（`AQUA_JB_RUNTIME_THREAD_DEBUG_LOG`）

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
| `WASAPI capture: MMCSS Pro Audio task registered` / `AvSetMmThreadCharacteristicsW failed`     | 采集线程入口                                     | 线程优先级注册结果               | 采集线程是否拿到 Pro Audio 优先级（拿不到就更容易饥饿）                                         |
| `WASAPI capture: data discontinuity (N frames follow)`                                         | 采集循环                                         | engine 上报断流                  | 切歌/流重建时引擎给的官方信号                                                                   |
| `WASAPI capture: timeline compensation start/end`                                              | 采集循环                                         | 进入/退出静音补偿                | 采集侧"欠账补静音"的起止与规模（`deficit` / `synth` 帧数）                                      |
| `WASAPI capture: starvation episode ended after Nms`                                           | 采集循环 / 线程退出                              | 饥饿 episode 结束                | 饥饿持续时长；`(thread exit)` 后缀表示是退出时结算的                                            |
| `AAudio playback data callback exception` / `callback returned N frames, but only M requested` | data callback                                    | 回调抛异常 / 契约被违反          | 同 WASAPI 回放，AAudio 侧的等价点位                                                             |
| `AAudio playback: dispatching runtime error event:`                                            | `report_fatal_once()`（可由 data callback 调用） | 运行期致命错误投递               | 每条流至多一行（`fatal_reported_` 保证），定位错误进入恢复链的时刻                              |

### B. 控制面门内（`AQUA_JB_CONTROL_THREAD_DEBUG_LOG`）

| 日志前缀                                                          | 位置                         | 何时出现                           | 排查用途                                                                                                                                                                            |
|-------------------------------------------------------------------|------------------------------|------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `TargetController change:`                                        | `TargetController::update()` | target 变化时**每次**              | 事件驱动全量：`desired` 与 `current` 的差、`margin` 两项各自值、`src`（`kJ`/`stall_peak`）、`floor_bind`/`cap_bind`、`penalty`、`path`、`fall_room`、`dwell_left`                   |
| `TargetController steady:`                                        | 同上                         | target 不变时，每 5s 一行          | **稳态下控制器在做什么**：`path=steady` = desired 真的等于 current；`path=dwell_lock`/`fall` = 被 dwell/限速按住；`path=deadband` = 被死区吞掉。7↔8 抽动类问题就靠它区分            |
| `JitterEstimator stall:`                                          | `observe()`                  | 每次判定为 stall                   | `gap` / `threshold`（含包周期倍数）/ `peak before -> after` / 生效的 `decay`。排查"峰值为什么挂这么久"必看                                                                          |
| `JitterEstimator anchor established (first packet):`              | `observe()`                  | 每流首包                           | 观测窗口的起点（开局 ~60ms 收敛期的第一块拼图）                                                                                                                                     |
| `JitterEstimator J milestone:`                                    | `observe()`                  | 第 1/16/64/256 个样本              | `J` 从 0 到收敛的轨迹（含 `transit` / `base` / `interval` / `peak`）                                                                                                                |
| `JitterEstimator transit anchor rebuilt (sender timeline reset):` | `observe()`                  | timestamp 非单调                   | 发送端时间轴重置：本包不进 J，只重建锚点。排查"J 为什么突然归零/不涨"                                                                                                               |
| `JitterEstimator reset on SSRC change:`                           | `observe()`                  | SSRC 变化                          | 新流接管：旧时间轴全部作废（含重置前的 `packets` / `stalls` / `J` / `peak`）                                                                                                        |
| `ClientRuntime startup anchored (startup -> steady):`             | arrival observer             | pre-roll 完成、`play_seq` 首次非 0 | 启动窗口与稳态的分界；同时给出锚定瞬间的 `lead` / `target` / `used` / `water` / `jit`                                                                                               |
| `JitterBuffer reanchor probe:` / `(pre-start)`                    | `JitterBuffer::push()`       | 远超前距离达到容量                 | **为什么 reanchor / 为什么没 reanchor**：`distance`、`gap_from_highest`、`min_gap`、`capacity`、结论。未达断裂缺口（环形满溢）时分母为 `JB_CONTROL_LOG_REANCHOR_PROBE_EVERY` 的节流 |
| `JitterBuffer reanchor sanity reject:` / `(pre-start)`            | `JitterBuffer::push()`       | 跨度超过 `max_span`                | 荒谬请求被拒的具体跨度（`span_frames` vs `max_span`）                                                                                                                               |
| `ClientRuntime push rejected (first):`                            | arrival observer             | 本会话**第一个**被拒的包           | 精确定位第一个被拒的包（`seq` / `play_seq` / `highest`），不必等 1s diag 行                                                                                                         |
| `ClientRuntime push rejected:`                                    | 同上                         | 之后每秒最多一行（有变化才打）     | 拒绝原因分布增量：`+late` / `+busy` / `+invalid` / `+sanity` 与累计值。`busy` 持续涨 = 结构性配置问题，见 `configuration_reference.md` 的 2/3 上限                                  |

### C. 不设门的运维日志（只看 `--log-level`）

| 日志前缀                                                                            | 何时出现                       | 备注                                                                                                 |
|-------------------------------------------------------------------------------------|--------------------------------|------------------------------------------------------------------------------------------------------|
| `CLI config:`                                                                       | 启动期                         | 完整生效配置（含 server / 播放设备 / 心跳）                                                          |
| `CLI effective JB options (src: cli=explicit, default=built-in):`                   | 启动期                         | 7 个 `--jb-*` 数值旋钮的最终值与**来源**，一行回答"用户到底传没传参"                                 |
| `ClientRuntime adaptive target controller:`                                         | 启动期                         | controller 的全部生效参数（含 `stall_cap` / `stall_decay` / `stall_threshold`，0 值会如实显示）      |
| `JitterBuffer created:`                                                             | 启动期                         | JB 几何与固定模式水位带                                                                              |
| `ClientRuntime adaptive target:`                                                    | target 变化时                  | **既有的** target 变化行（水位带、`src`、夹持状态）。控制面门关闭时它仍然在，是 Release 下的主要抓手 |
| `ClientRuntime network stall:`                                                      | 每次 stall                     | **既有的** stall 行。`stall_peak_ms` 与 `- not into J, handled ...` 说明它没进 J                     |
| `ClientRuntime geometric floor recalibrated ...`                                    | 设备切换导致 callback 几何变化 | 几何地板被校正（`min_target` 前后值）                                                                |
| `ClientRuntime push rejected` 之外的 `push_rejected_*` 计数器                       | 1s diag 行                     | 稳态汇总口径，见 `../diagnostics.md`                                                                 |
| `WASAPI playback started:` / `WASAPI capture started:` / `AAudio playback started:` | 启动期                         | 后端实际协商结果                                                                                     |

## 排查指引

| 症状                            | 先看哪行                                                                                              | 再动哪个旋钮                                                                                                                                      |
|---------------------------------|-------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| `target` 钉在高位不下来         | `JitterEstimator stall:` 的 `peak`、`TargetController steady:` 的 `stall` 项与 `path`                 | `--jb-stall-decay`（调大 = 更快忘记）、`--jb-stall-peak-cap`（调小 = 最多推多高）                                                                 |
| `target` 在相邻两格之间反复横跳 | `TargetController change:` 的 `src` 与 `path`                                                         | 先确认是 `kJ` 摆动还是 `stall_peak` 摆动；前者看 `--jb-jitter-gain`，后者看 `--jb-stall-*`。`dwell_left` 非 0 说明锁跌正在生效                    |
| `target` 卡在 desired−1 不动    | `TargetController steady:` 的 `path=deadband`                                                         | 死区默认 0（`configuration_reference.md` 说明了为什么必须恒 0）                                                                                   |
| 持续欠载（`underrun_ratio` 高） | `JitterBuffer underrun enter` 的水位/target + `ClientRuntime network stall` + `JitterEstimator stall` | 先分清是"抖动超出预测"（`--jb-jitter-gain`）还是"stall 尾部被门剔除"（`--jb-stall-peak-cap`）；`--jb-underrun-penalty 0` 可确认反馈项是否在起作用 |
| `busy` 拒绝率异常高             | `ClientRuntime push rejected:` 的 `+busy` + 1s diag 的 `used/target`                                  | 结构性：`target` 顶穿 2/3 上限（`configuration_reference.md`）。先降 `--jb-jitter-gain`，不要动容量                                               |
| 播放头停住、声音全无            | `JitterBuffer reanchor hold-stuck fallback:`、`water adjustment: REANCHOR`                            | 看 `pulls` 是否顶到阈值、`lead` 是否卡住；`min_gap` 相关判定见 `JitterBuffer reanchor probe:`                                                     |
| 怀疑 reanchor 该触发却没触发    | `JitterBuffer reanchor probe:` 的结论字段                                                             | `no-gap: ring overflow` = 交由 `deadline-high DROP` 兜底，属预期                                                                                  |
| "刚才那段是掩盖音还是真音"      | `JitterBuffer conceal enter/saturated/exit`                                                           | `--jb-no-conceal` 做听感 A/B                                                                                                                      |
| 怀疑参数没生效                  | `CLI effective JB options` 的 `cli` / `default` 后缀                                                  | ——                                                                                                                                                |
| 参数传了但行为没变              | `ClientRuntime adaptive target controller:` banner 的 0 值                                            | `--jb-fixed-target` 打开时控制器根本不建，见 `configuration_reference.md`                                                                         |

## 日志节奏常量（不是调参项）

以下常量只影响日志密度，改它们不会影响音频行为，都定义在 `buffer_config.h`：

- `JB_CONTROL_LOG_SUMMARY_INTERVAL_MS`（5s）：`TargetController` 稳态摘要的节流周期；
- `JB_CONTROL_LOG_REJECT_INTERVAL_MS`（1s）：push 拒绝汇总的节流周期（首次不节流）；
- `JB_CONTROL_LOG_REANCHOR_PROBE_EVERY`（128）：reanchor 探测日志在"未达断裂缺口"分支的节流分母。

## 使用约束

两个调试宏都 **只能短时间复现问题时开启**，不得作为生产基线：RT 宏开启后直接违反实时契约；控制面宏会把 push strand
的每包处理路径变成带锁的字符串格式化。排查完请回到 release preset（两个宏都 OFF）。
