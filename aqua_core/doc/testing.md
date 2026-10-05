# 测试与回归

## 1. 测试原则

测试目标不是只验证“能播”，而是验证边界契约：序号、容量、并发发布、停止顺序、格式拒绝、网络异常、回调生命周期。

## 2. Audio model

覆盖：

- `AudioFormat` 合法性与 overflow
- frame/byte 对齐
- `AudioFrame` well-formed

## 3. Packetizer / Queue

必须验证：

- 变长 block 跨多个 slot；
- pending 半帧保留到下一次 push；
- unaligned block 被拒绝；
- sequence 连续；
- queue 满时 drop newest；
- consumer callback 返回后才 release slot；
- wake hint 不是队列状态本身。

## 4. JitterBuffer

重点回归：

- pre-roll 恰好 startup_level 边界；
- 未启动远跳与 reanchor；
- 运行中远跳；
- late frame；
- slot collision；
- duplicate sequence；
- 缺帧静音；
- output 跨 slot；
- Fill warning 慢放校正（重播 READY slot）与 target 终止；
- Drop step 增长；
- deadline-high；
- reanchor 后陈旧 Ready 清理；
- `advance_slot()` 的先推进 play_seq 再回收 slot；
- reanchor hold-stuck 5-pull fallback；
- sanity jump rejection。

### 4.1 控制律离线回放（`jitter_control_replay_test.cpp`）

**改 target 控制参数前先跑它**，不要直接上现场。它把合成链路喂进**真实的**
`JitterEstimator` + `TargetController` + `JitterBuffer`（不是重实现：公式、限速、风暴、
带几何全部来自生产代码，带几何经 `apply_adaptive_bands()` 与 ClientRuntime 同源），
虚拟时钟 + 固定 seed + 扫 16 个相位取最坏 → 确定性、无 sleep。

看场景表：

```bash
aqua_jitter_buffer_tests --gtest_filter=*ScenarioTable*
```

覆盖：干净链路 / 1ms / 3ms 抖动 / 1% 丢包 / **1% 成串丢包**（Gilbert-Elliott，平均连丢 6 包）/
35% 断流风暴 / **hold 风暴**（250ms 黑洞 + 回补 burst，delay-spike regime）/
**2s 完全断流**。每行给出 target 的 min/max/mean、静音占比、最长连续断流、
FILL/DROP episode、P99，以及决策层的路径占比（rise/fall/dwell/storm）与切换次数。

已钉住的不变量：

- target 永远落在 [几何地板, 2/3 容量]；
- 干净链路 target 贴地板（零欠载下限验证）；
- 丢包走 concealment 而非裸静音，**成串丢包**同样守预算；
- **splice 只改输出样本**：开/关 splice 时逐包 target 与路径序列必须逐位相同，
  输出只在 ≤2× 淡变长度的短窗内不同（实测 56 段 / 最大 63 帧 / 占 0.61% 样本）；
- 风暴期进 storm hold；
- **断流后残余被 stall 封顶挡住、且不锁死**（见下）。

### 4.2 断流后的残余水位（已知取舍，实测数据）

2s 完全断流的实测（最坏相位）：

```text
ref end:  target=7  tail_margin=2.92  stall_margin=0.00  eff_min=4
run end:  target=8  tail_margin=2.92  stall_margin=8.00  penalty=0.00
          stall_peak=1770ms -> 回落到封顶以下约需 174s
```

即：残余**来自 stall 峰值项**（不是欠载罚分——罚分上限 6 槽、0.5 槽/s，12s 内必清零），
幅度被 `JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS` 挡住（8 槽），但**持续时间与事故时长成正比**：
峰值按 `JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC`（10ms/s）线性衰减，2s 的峰值约 174s
才回落到封顶以下。相对链路正常稳态的增量是 **+1 槽（3.6ms）**。

这是设计取舍（`buffer_config.h` 该常量注释即写"大事故挂更久"），不是缺陷；
`OutageResidueIsCappedNotLocked` 把"封顶生效 + 不锁死"钉成断言。若将来要把
"事故后回落时间"纳入契约，需要单独决策（例如给 stall 峰值加时间上限，或对
超过阈值的峰值改用别的衰减律）。

### 4.3 同文件其他套件（一句话索引）

- `StormCapacitySweep`：容量 30/60/120 × 风暴四档。结论：纯丢失型风暴加容量逐字无效；
  hold 风暴下 cap120 让 churn 归零且 target 不涨（地板驱动）。
- `PresetLadderSweep`：min-target 3/6/9/12 × 四档链路。结论：min 6 以 +7ms 杀掉干净链路的
  DROP 抖动；丢包链上抬 min 有害（走 penalty 路线）；hold 风暴看容量不看 min。
- `HoldBurstPreservesOrder` / `HoldStormReproducesBurstPathology`：hold 模型保序回归
  （串行化释放）与病理签名（busy/reanchor 非零，区别于 loss 模型）。

### 4.4 真 trace 回放（`jitter_trace_replay_test.cpp`）

`AQUA_JB_TRACE=/path/to/client.log` 指向 `--jb-trace` 语料（JBT 行新旧格式都认），
同一份到达节奏跑 16 相位：`ReplayIsDeterministic`（同输入同输出）与
`SameTraceBoundsHold`（target 永不出 [floor, max] + 逐相位表）。未设置则跳过，
CI 常绿。拿现场问题做回归时，先把 trace 留下来，再谈复现。

## 5. UDP / Session

覆盖 malformed datagram、wrong type、payload size mismatch、wrong session、unexpected sender、heartbeat
establish/refresh、timeout reap、disconnect idempotence。

### 5.1 地址族覆盖矩阵

数据面逐族（v4 socket + v6 socket，共用端口）之后，"哪一族被服务、客户端选哪个槽"必须逐格锁定：

| 用例 | 套件 | 锁定什么 |
|---|---|---|
| `UdpDualStackTest.BindsBothFamiliesOnOnePortAndRoutesByFamily` | `aqua_net_tests` | 同端口两族共存；两族各自建 association（v4 与 v6 各一条真实 `UdpClient`）；一份 datagram 按族路由**同时**到达两族 |
| `UdpDualStackTest.OptionalFamilyBindFailureDegradesToSingleFamily` | 同上 | 单族绑定失败 ⇒ 降级为单族且仍能启动（INV-5 的宽松半边） |
| `UdpDualStackTest.StartRejectsWhenNoFamilyIsBound` | 同上 | 无族可服务时 `start()` 必须拒绝（不允许"绑定成功却收不到"） |
| `SessionManagerTest.PinnedFamilyRejectsCrossFamilyHeartbeat` | 同上 | 族粘性（INV-3）：首包钉族，跨族 heartbeat 被拒且不改动 endpoint |
| `SessionManagerTest.HeartbeatWithUndeterminedFamilyIsRejected` | 同上 | 族未观测到时不得写入（否则扇出会选错 socket） |
| `IpFamilyTest.*` / `GrpcPeerAddressTest.*` | 同上 | `::ffff:` 归一化、原生形态判定、gRPC peer 文本解析（**两族共用的独立套件**，不是同一个套件下的对照组） |
| `GrpcServerTest.RoundTripOverIPv6Loopback` | 同上 | v6 客户端在**两槽都有**时选到 `udp6` 槽（选槽规则 = INV-4 / §3.2；INV-7 说的是"槽存在 ⟺ 该族绑定成功"） |
| `GrpcServerTest.ConnectRejectsClientFamilyThatIsNotServed` | 同上 | 族准入：族不被服务 ⇒ Connect 期拒绝**且不留下 session** |
| `GrpcClientTest.ConnectRejectsMissingSlotForClientFamily` | 同上 | 空槽 ⇒ 客户端硬失败并回滚已建 session |
| `GrpcClientTest.ConnectRejectsCrossFamilyUdpAdvertisement` | 同上 | 客户端自证：槽内地址族不符即失败 |
| `GrpcClientTest.ConnectRejectsEmptyUdpAdvertisement` | 同上 | 空串不再是 sentinel（不允许退化成"发向自己"） |
| `ServerRuntimeConfigTest.*` | 同上 | Core 层的配置期校验：跨族/通配通告/zone/`none` 冲突/双关/`udp_port=0`（**清单以源码为准，本文不写死条数**） |
| `IpFamilyTest.NativeFamilyRejectsV4MappedForBothFamilies` | `aqua_net_tests` | `is_native_family` 与 `effective_family` 的**差别**：v4-mapped 归一化算 v4、但原生两边都不认 |
| `GrpcPeerAddressTest.ParsesFullyEncodedPeerWithEncodedPortSeparator` | 同上 | 整段 percent-encode（连端口冒号 `%3A`）的 peer 也必须解析出族（否则服务端准入检查静默失效） |
| `ServerRuntimeConfigTest.RejectsStartWhenPinnedIpv4BindFails` / `…Ipv6BindFails` | 同上 | INV-5**严格**半边：pin 的族绑定失败 ⇒ 启动失败；并断言**另一族确实绑上了**（证明真的走到了 bind，否则用例会假绿） |
| `UdpDualStackTest.CrossFamilyHeartbeatIsRejectedOnceFamilyIsPinned` | 同上 | INV-3 **线上层**：同 id 跨族 heartbeat 被拒、endpoint 与族都不被覆盖、`heartbeat_rejected` 增长 |
| `GrpcServerTest.ConnectRejectsV4ClientWhenOnlyV6IsServed` | 同上 | 族准入镜像方向（**最初那个 bug 的形状**）：控制面通、数据面只服务 v6 ⇒ v4 客户端连接期被拒且 0 session |
| `ServerRuntimeConfigTest.RejectsV4MappedRpcIp` / `…UdpIpv6BindWithV4MappedLiteral` / `…WildcardAdvertisedIpv6` / `…ZeroAdvertisedUdpPort` | 同上 | 另外四条 Core 配置期校验（控制面原生形态、v6 槽 v4-mapped、v6 通配通告、通告端口 0） |
| `UdpLoopbackTest.TransportRejectsV4MappedLiteralOnBind` / `…AsRemote` | 同上 | **最底层加固**：`UdpTransport::bind()` / `set_remote()` 自己拒绝 `::ffff:a.b.c.d` —— 此前只有上层校验，漏在这一层就会开出 AF_INET6 socket（`effective_family` 会把这类地址判成 V4 并放行，所以判定必须用 `is_native_family`） |
| `IpFamilyTest.NativeFamilyRejectsV4MappedForBothFamilies`（含 `Unknown` 断言） | 同上 | `is_native_family(addr, Unknown)` 恒为假 —— 与 `FamilySet::contains(Unknown)` 同约定，避免漏判时静默放行 |
| `ClientRuntimeConfigTest.EndpointValidator*` / `StartRejectsWildcardServerAddress` | 同上 | INV-8 反向用例：通配（`0.0.0.0` / `::`）、v4-mapped、非字面量、`rpc_port=0` 一律拒绝。**直接测 `validate_client_endpoint()`** 而不是只断言 `start()` 返回 false —— 后者在没有 server 时本来就会失败，属假绿 |
| `ServerRuntimeConfigTest.ControlPlaneReachableFamiliesFollowTheBindAddress` / `…CoverageSeparatesUnservedFromIdle` | 同上 | INV-7 的 pin 例外：通配 ⇒ 两族可达、具体 ⇒ 只那一族；`unserved`（连得上却被拒）与 `idle`（绑了却没人连得上）必须分开；地址解析不出来 ⇒ 空集且**不致命**（fail-open） |
| `ServerRuntimeConfigTest.RejectsStartWhenNoReachableFamilyIsServed` | 同上 | 上层集成：`--rpc-ip ::1` + `--udp-ipv6 none` 以前能正常启动、现在必须拒绝；并断言 `udp_served_families()` **为空**——证明检查前置于 bind（否则本用例的前提不成立） |
| `SessionManagerTest.PendingFamilyCountsTrackTheConnectTimeFamily` | 同上 | `create_session(expected_family)` + `pending_family_counts()`：未建连的 session 按期望族计数，建连后从 pending 移出（与 `connected_family_counts` **互斥**，否则"有人在等"会被重复计数） |
| `UdpDualStackTest.HeartbeatCountersAreSplitByFamily` | 同上 | INV-6 逐族心跳：v4 有流时 `heartbeat_received(V6)` 必须为 0（合计口径看不出这件事），且 合计 == v4 + v6 |
| `IpFamilyTest.LinkLocalScopeIsBorrowedFromTheReferenceAddress` | 同上 | zone 规则（INV-10 的客户端半边）：link-local 目标无 zone 时借用参考地址的 zone，补出**数字 ifIndex**（asio 的 Windows 解析器只认十进制）；借不到则返回可操作错误 |
| `ClientRuntimeConfigTest.EndpointValidatorRequiresZoneForLinkLocalRpcIp` | 同上 | `--rpc-ip` 是 link-local 就必须带 zone：缺了连控制面都发不出去，也没有 zone 可借给数据面 |
| `ServerRuntimeConfigTest.RejectsLinkLocalRpcIpWithoutZone` | 同上 | 控制面绑定与数据面 bind 同一条规则（对称） |
| `GrpcClientTest.ConnectRejectsLinkLocalSlotWhenNoZoneCanBeBorrowed` | 同上 | 集成：服务端下发 link-local 端点、客户端也没有 zone ⇒ Connect 期响亮失败并回滚 session（zone 不能上 wire，这是客户端唯一的拦截点） |

未覆盖（已知缺口，**自动化**层面）：
`UdpClient` 的**单测**都走 `127.0.0.1`（v6 的端到端在 `UdpDualStackTest` 里跑真实 `UdpClient`，见上表）；
link-local（`fe80::…%<ifIndex>`）只在地址解析与借用逻辑层面有测试（含上面 4 条），
**缺真实链路的端到端**："借用 zone 之后真的能拨通 link-local 对端"无法在本机验证
（需要一块真实 link-local 网卡与对端），因此那句承诺目前只有纯函数级证据；
C API / JNI 那条路只覆盖"参数落到 `ClientRuntimeConfig`"，没有 v6 地址的端到端连接用例。
端口覆盖（`--udp-force-port` / `--udp-advertise-port`）只有 CLI 探针验证**接线**，
本地没有转发器，所以"覆盖后真能收到音频"依赖部署环境（见 `operations_and_troubleshooting.md`）。

### 5.2 CLI 端到端探针（手动、可复跑）

单测到 `SessionManager` / `UdpServer` 为止；"两族客户端真能同时跑起来并各自收到音频"必须用真实二进制。
本轮用本地脚本跑过下列组（脚本不随仓库交付，只作验证手段）：

| 组 | 场景 | 期望与锁定点 |
|---|---|---|
| A–G | 默认启动 / 同绑两族具体地址 / `none` 关族 / 逐族通告 / v4-mapped 拒绝 / force-port / advertise-port | 前五组各自断言"监听面（netstat 两族 socket）+ 两端 association"；F/G 额外锁定 `--udp-force-port` **只覆盖端口、不动槽与族**，以及 `--udp-advertise-port` 下发的确实是通告端口（客户端拿到 50998 而非绑定端口 50027） |
| H | 默认启动 + v4/v6 **并发**在线 16s | `served=v4+v6`，`v4tx`/`v6tx` 均非零且 `txerr=0`；两端各自 `association established` 且 `af`（音频接受计数）非零；客户端退出后会话全部回收（`rem=exp=2`、`act=0`） |
| I | 控制面无 server | 3s 内明确失败并退出（不挂死） |
| J | 控制面端口被占用 | 启动失败 exit=1，且逐族 bind 失败原因可读 |
| M | 3×v4 + 1×v6 并发 | `crt=con=4`、`v4tx ≈ 3×v6tx`、`txerr=0`（无 `EAFNOSUPPORT` = 扇出按族路由正确） |
| N | `--udp-ipv4/-ipv6 ::ffff:127.0.0.1` | 配置期拒绝，exit=1 |
| O | 只服务 v6（`--udp-ipv4 none`） | v6 客户端正常建立 association；v4 客户端 Connect 期被拒且 **0 session** |
| Y | INV-7 pin 例外 + 逐族收包健康（`--rpc-ip ::1` / `--rpc-ip 0.0.0.0` + `--udp-ipv6 none` / `--rpc-ip 127.0.0.1` + 默认两族 / 线上诊断行） | Y1 拒绝启动 exit=1 且 **netstat 无任何监听**（证明校验前置于 bind）；Y2 v6 不可达但 v4 仍服务 ⇒ 启动 + warn「v6 clients will be rejected at Connect」；Y3 被服务却不可达的 v6 socket ⇒ 一行 debug「stay idle」；Y4 诊断行 `v4hb` 非零、`v6hb` 恒零（合计口径看不见的单族静默） |
| Z | link-local zone 规则 + 启动日志口径（客户端 `--rpc-ip fe80::1` / `fe80::1%24`；服务端 `--rpc-ip fe80::1`、`--udp-advertise-ipv6 fe80::1`；单族降级） | Z1/Z3 无 zone 的 link-local 在**客户端与服务端都于配置期拒绝**，消息给出 `%<ifIndex>` 写法；Z2 带 zone 通过校验并把 `[fe80::1%24]` 送去拨号（之后的失败是连不上，不是 zone 错误）；Z4 link-local 通告**仍被接受**（zone 不上 wire，由客户端借用）；Z5 先占住 `[::]:P`、再让同端口两族通配的实例启动 ⇒ v6 降级，启动行 `served=v4` 且 `advertise(v6)=not-served`（修复前会打印配置地址，与 `served=` 自相矛盾） |

⚠️ 探针的停止动作必须用 `taskkill //F` 兜底：Git Bash 的 `kill` 对 Windows 原生进程**不可靠**，
会留下残留进程锁住 exe，让下一次链接报 `LNK1104: 无法打开文件 …exe`（诊断特征：被"kill"的进程日志里
连 shutdown 行都没有 = 信号根本没送达）。

## 6. Runtime

覆盖：

- 非法配置拒绝；
- backend 缺失；
- Connect 无效 response；
- stream geometry overflow；
- payload 超 MTU；
- UDP 启动失败；
- heartbeat 启动失败；
- playback start 失败；
- stop 幂等；
- async callback 晚到时 CallbackGate 不 use-after-free；
- Server reaper stop 不留下 timer work。

## 7. 设备切换测试

切换事务用 mock 后端 + mock 设备管理器覆盖（`tests/audio/*_manager_test.cpp`），不依赖真实音频设备：

- 候选链：一次成功（`Switched`）、回滚（`RolledBack`）、落系统默认（`FellBackToSystem`）、链耗尽（`Fatal`）；
- `Fatal` 是终态：后续 restart 被拒且不触碰后端；
- 重试预算：窗口内前 3 次成功，第 4 次直接 `Fatal`（`start_attempts` 不增长）；
- 格式钉死：候选收到的请求格式等于会话格式；backend 违约（info 不符）时候选按 `FormatUnsupported` 处理；
- 回调活跃期 restart：无死锁、无双重生产/消费（`max_concurrent_callbacks == 1`）；
- 共享预算：错误驱动与默认跟随的 restart 合并计数；
- **路由未稳定预算**（`PlaybackManagerSettleTest`）：`ImmediateStreamDeathUsesSettleBudgetNotDeviceLossBudget`
  钉住"流刚起来就死走 settle 预算、重试期间**不**消耗设备丢失预算"；`SettleRestartIsThrottledWithoutEscalatingToFatal`
  钉住"节流命中时不改任何状态、不升级成 Fatal"（由 supervision tick 稍后再驱动）；
  `SettleExhaustionDegradesToSystemDefaultNotFatal` 钉住"预算耗尽 = 降级到系统默认而非 Fatal，且 sticky 意图保留"；
  `SettleDegradeIsBoundedByDeviceLossBudget` 钉住"降级借用设备丢失预算封顶，三级都耗尽才 Fatal（正好 3 次降级）"；
- **notify 消费诚实性**（`PlaybackManagerDevicePollTest.ThrottledEagerRestartReportsNoTransaction`）：节流空操作报
  `false`（`switch_seq` 未变），service 不清错误；预算耗尽 `Fatal` 报消费（`AutoFollowConsumesSharedBudget` 第 4 次仍为 `true`）。
  `AdoptUserIntentPreservesStickyAfterFallback` 钉住起步回退补装后 `User` 归属与 sticky 意图；
- **poller 空集**（`DeviceSetPollerTest`）：`EmptyEnumerationIsValidEmptySet`（原始空 = 有效空集基线）与
  `RemovalToEmptyIsReported`（`[a]`→`[]` 报 `removed=[a]`）；合成空 id 条目仍是无信息（`SyntheticEmptyIdIsNotInformation`、
  `NoInformationDoesNotErasePendingDebounce` 不变）；
- **事件归属**（`PlaybackManagerProvenanceTest`）：`StaleGenerationEventIsDropped` 与
  `EventDuringTeardownIsDroppedByUnclaimedGeneration`——两者都同时断言**结果**（事件不到应用回调、不触发第二次
  restart）与**机制**（`stale_events_dropped() == 1`）。

mock 后端为此提供两个钩子：`simulate_stream_death(AudioError)`（把流标死 **并**用当前 generation 打戳投递事件——
两半都必要，因为 settle 分类的判据之一是 `!is_running()`）与 `fire_event_with_generation(AudioError, StreamGeneration)`。

capture 侧的同一回归由
`CaptureManagerTickTest.StaleErrorDuringFollowTransactionDroppedByGeneration` 守着（原名
`...ObservesSwitching`）：守的东西没变，**拦截点**从 runtime 的 Switching 时间窗换成了 manager 的 generation 过滤。

时间线连续性（restart 前后 seq 单调、session 不重建）由实现结构保证：切换事务只调用管理器自身方法，packetizer / network /
session 不在其可达范围——属于代码评审项而非断言项。

## 8. 平台测试

WASAPI 测试要与 domain test 分开看：domain tests 验证纯 Core 语义，WASAPI tests 验证 COM、event、buffer/padding、设备错误和真实
callback 生命周期。

新增 AAudio 时，优先复用 domain test 集，不要把协议/缓冲测试复制成 Android 专用版本；Android 专用测试只覆盖 AAudio adapter
和 JNI ABI。

## 9. 测试目标与运行

测试不是单一可执行，而是按模块拆成的多个 gtest 目标：

| 目标                               | 内容                              | 平台       |
|------------------------------------|-----------------------------------|------------|
| `aqua_tests`                       | logger                            | 全         |
| `aqua_diagnostics_tests`           | diagnostics                       | 全         |
| `aqua_net_tests`                   | gRPC / session / UDP / 格式转换   | 全         |
| `aqua_audio_tests`                 | AudioFormat / AudioFrameQueue / DeviceSetPoller | 全         |
| `aqua_audio_packetizer_tests`      | packetizer                        | 全         |
| `aqua_jitter_buffer_tests`         | JitterBuffer（含边界与回归）      | 全         |
| `aqua_playback_manager_tests`      | PlaybackManager 切换事务          | 全         |
| `aqua_capture_manager_tests`       | CaptureManager 切换事务           | 全         |
| `aqua_capi_test`                   | C API（需 `AQUA_BUILD_C_API=ON`） | 全         |
| `aqua_wasapi_device_manager_tests` | WASAPI 设备解析                   | 仅 Windows |
| `aqua_wasapi_capture_tests`        | WASAPI 采集                       | 仅 Windows |
| `aqua_wasapi_playback_tests`       | WASAPI 回放                       | 仅 Windows |

全部经 `gtest_discover_tests` 注册，因此按用例粒度跑：

```bash
ctest --preset windows-x64-debug          # 全量
ctest --preset windows-x64-debug -R CaptureManager   # 按名字过滤
ctest --test-dir cmake_build/windows-x64-debug -C Debug --output-on-failure
```

test preset 只有 Windows / Linux / macOS 六份，Android 没有（Android 只构建 `aqua_capi`，测试跑在主机侧）。 完整构建与 preset
说明见 `build_and_release.md` 与仓库根 `BUILD.md`。
