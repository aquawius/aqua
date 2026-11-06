# 已知取舍（known issues）

本文记录**设计之初有意接受**的取舍：现象、放弃的另一边、接受的理由、重开条件。
行为描述以代码为准；数值以 `buffer_config.h` / `udp_config.h` / 各 manager 头文件为准。
与故障排查手册的关系：`operations_and_troubleshooting.md` 回答"坏了怎么看"，
本文回答"为什么当初这样定"。

## 1. 切换链末重试阻塞 ioc 最长 2s

`PlaybackManager::switch_to` 在候选链耗尽且末错误瞬时（`DeviceUnavailable` /
`DeviceDisconnected` / `BackendFailed`）时，同步重试上一设备（4 次，退避
200ms×n，累计约 2s）。错误驱动恢复跑在 ioc 线程，这 2s 内 UDP 心跳停摆，吃掉约
40% 的 5s 会话预算。

接受理由：这是"换设备别把整条连接带崩"的最后一道保险；可达窄——`DeviceNotFound` /
`FormatUnsupported` 直接跳过重试，真拔设备不走这条路；2s 仍在 5s 预算内。
重开条件：出现心跳被饿死导致的 Degraded 实测，或做 pending-retry 状态机
（记截止时刻后返回，由 supervision tick 再驱动——改动涉及吸收点时序，需独立评审）。

## 2. 跟随与错误共享 10s/3 预算

错误驱动 restart、内部跟随（tick/快照/自动切回）合并计数，风暴期打满即 `Fatal`。
路由抖动已由独立 settle 预算分流，不烧这里的额度；1s 合并窗口先吸收蓝牙连接风暴。
重开条件：出现"正常插拔风暴被判 Fatal"的实测。

## 3. `AAUDIO_ERROR_UNAVAILABLE` 归入 `BackendFailed`

该码语义含混（设备不可用 vs 请求配置不可用），现判不可恢复（→ `Degraded` 停会话），
而非 `DeviceUnavailable`（→ 恢复）。切换路径不受影响（瞬时重试三码同权），差异只在
error 回调这一条路上。接受理由：若真表示配置不可用，重试只是无谓打转；error 回调日志
已同时打印原始码，可观测。重开条件：实测抓到该码对应可恢复场景。
**补（2026-10-11）**：启动路径已按该重开条件做了一半 —— `openStream` 返回此码且本次请求了低延迟时，
先用 `PERFORMANCE_MODE_NONE` 重开一次，仍失败才照旧判 `BackendFailed`（见 `aaudio_backend_design.md` §2）。
**error 回调那条路（运行期流死）未变**，仍直接判不可恢复。若重试后仍失败，说明该码在这里确实对应
不可恢复场景，本条可以就此收口。

## 4. settle 常量来自单一样本

`kRouteSettleWindow` 400ms / `kSettleRetryInterval` 200ms / `kMaxSettleRestarts` 8 /
`kSettleWindow` 5s 由一份蓝牙切换日志定标。接受理由：有实测总比拍脑袋强，且降级路径
（耗尽转系统默认）兜住了"样本外更长的路由转换"。重开条件：多机型实测中"stream died Nms"
 routinely 超过 400ms——只加宽窗口，其余不动（测试 `SettleExhaustionDegrades…` 锁定形状）。

## 5. Android 全集推送的跟随副作用

`present_ids` 是未过滤 sink 全集（`active_gone` 判断必需），而 Android 的
`default_device` 是合成空 id，于是 `None` 归属下**任何**新 sink 到达即跟随——含 SCO
等通话类型。接受理由：单次多余 restart（预算封顶），远好于子集推送造成的每快照误判
`Fatal` 循环；core 拿不到设备类型（id 是不透明 `android:N`），分不清媒体与通话路由。
重开条件：JNI 透传设备类型（C 边界变更）或系统路由可查询。

## 6. 空集与枚举失败不可分

`DeviceSetPoller` 依据原始项是否含空 id 区分"合成条目（无信息）"与"真零设备（有效空集）"，
但 WASAPI `enumerate()` 以返回值区分不了"真零设备"与"COM/枚举失败"——两者都是空向量。
接受理由：失败连两次才上报（去抖），偶发失败几乎到不了决策；真零设备必须能上报（否则
capture"主动 Fatal"保证失效）。重开条件：`enumerate()` 改 `expected`（动接口与全部 mock）。

## 7. capture 只有 `DeviceDisconnected` 触发切换

运行期 `BackendFailed`（如 `SERVICE_NOT_RUNNING`）走 `Degraded` 停 server，不进
restart。接受理由：后端服务死亡不是换个端点能解决的，重试只会拖长无声。重开条件：出现
可恢复的 `BackendFailed` 实测。

## 8. session_id 不是每会话强随机

`thread_local mt19937_64`（`random_device` 播种一次），输出可逆。接受理由：威胁模型只有
局域网被动观察者；注释与协议文档已如实说明，不再声称 CSPRNG。重开条件：威胁模型升级
（公网/主动攻击者）——届时换系统熵源逐 id 生成。

## 9. ~~空/非法 UDP 通告静默回退~~（已关闭：改为硬失败）

**现状**：Connect 选中的槽里，地址为空串 / 非法字面量 / 端口 0 或超 `uint16` 上限，都直接让整笔 Connect
失败并 best-effort Disconnect 回滚；唯一保留的"回退"是通配 sentinel（改用客户端自己的 `rpc_ip`，族必然一致）。

理由：回退只会把 server 侧的错误悄悄变成"发向另一个地址"，故障被推迟到客户端收不到 ACK 才暴露，而且那时
双方都不知道真正原因。现在只保留**有明确语义**的 sentinel 回退，其余一律响亮失败。
（旧行为记录：空串/非法地址回退到 gRPC `rpc_ip` 且不断连。）

## 10. ~~`UdpClient::open` 的 IPv4 死分支~~（已关闭：删除兜底 open）

**现状**：`UdpClient::start_receive` 不再在"remote 已设但 socket 未 open"时兜底调用
`UdpTransport::open()`（它硬编码 `0.0.0.0:0`，会把 v6 客户端锁死在 AF_INET）——该状态视为调用序错误，
返回 `NotOpen` 响亮失败。打开 socket 是 `set_remote()` 的职责（按远端地址族选 IPv4/IPv6）。

残留缺口：`UdpTransport::open()`（无参）仍是 v4 专用（硬编码 `0.0.0.0:0`；需要 IPv6 请直接 `set_remote()`，
它会按远端族选 socket）。**IPv6 的端到端已不再算缺口**：`UdpDualStackTest` 里两条真实 `UdpClient`
（一条 v4、一条 v6）各自建 association 并收广播，跑在 `ctest` 里（只有 *单测* 仍全是 `127.0.0.1`）。
重开条件：需要在没有远端地址的情况下预开 socket 时。

**已加固（2026-10-04）**：`UdpTransport::bind()` / `set_remote()` 现在自己拒绝 `::ffff:a.b.c.d` 形态
（返回 `InvalidEndpoint`）。此前只有上层（`ServerRuntime` / `UdpServer` 单族适配器 / `GrpcClient`）校验，
而本层才是"地址原样交给套接字层"的最后关口 —— 漏在这里就会开出一条 AF_INET6 socket。

## 11. ~~`connected_scratch_` 单调用方假设~~（已关闭：扇出集合改为不可变快照）

原问题：`UdpServer` 复用成员 scratch 做快照，无锁、无断言，仅靠注释约束"同一时刻只有一个
dispatcher/owner"。这条约束在旧实现里是**真实存在的**：scratch 是可变成员，第二个并发
broadcast 调用方会直接踩坏它。

已关闭：`3277ba1` 把扇出集合换成 `SessionManager::connected_snapshot()` —— 读侧只拷一个
`shared_ptr`、无分配、**且不再有单调用方约束**，成员 scratch 也随之删除。真正的收益不是并发
安全，而是每包少一次 `shared_lock` + 一次全 map 遍历（那条锁与 `on_heartbeat` 的写锁
同源，发送线程被它挡住就会变成 pacing 抖动 → 对端 target 抬高）。

**2026-10-10 修正：发布点不再是 `std::atomic<std::shared_ptr<T>>`。** 那个类型在
**libc++（Android NDK）里没有实现**——名字落回 `std::atomic<T>` 主模板，
`static_assert(is_trivially_copyable<T>)` 直接让 Android 编译失败（NDK 30 / LLVM 21，
C++20 与 C++23 皆然，已用 NDK clang++ 单独复现）；MSVC 的 STL 有实现，所以 Windows
双配置一直是绿的，**这个错误只在 Android 出现**。现改为一把**专用**
`published_connected_mutex_` 保护那个 `shared_ptr`：临界区只有一次指针赋值/拷贝
（纳秒级），与 `mutex_` 无嵌套环（写者 `mutex_` → 本锁，读者只取本锁）。要保住的不变量是
"**读侧不碰 session 表的 `mutex_`**"而不是"任何形式的锁"；
`std::atomic_load/atomic_store(shared_ptr*)` 这条替代路已在 C++20 弃用（C++26 移除），
不适合新代码。

仍然成立的部分：`broadcast` 的唯一调用方仍是 AudioNetworkDispatcher 的worker 线程，
而那是**发包时序敏感**的 pacing 线程；它的抖动会经对端 jitter 估计反馈回target。
`udp_server.h` 的注释已按这个口径更正（原文写的"非实时线程调用"是错的）。

## 12. Kotlin 容错优先于排障信号

`AquaRouteMode.fromCode` 等遇未知码回落 benign 默认；C 侧查询入口已加 magic 浅校验，
JNI 非法句柄传 null。接受理由：App 不因诊断抖动断连；野句柄在 C 侧拦，Kotlin 侧显示保守值。
重开条件：排障需要时把回落改成"未知"标签（加枚举值，C/Kotlin/诊断三处同步）。

## 13. 重连固定 3s 退避、无上限

`RECONNECT_DELAY_MS` 统一快/慢失败节奏，保证"已停止"可见；无指数退避、无次数上限。
接受理由：core 契约"终态即停"，重连是 UI 层行为；停止期可见优先于退避优雅性，用户断开即停。
重开条件：自动重连风暴实测（如 server 长时间不可达时的行为投诉）。

## 14. Opus 不做（局域网无意义）

已评估：Opus 省带宽，而本项目的瓶颈是抖动/断流不是带宽；最小 2.5–5ms 帧长加前瞻反而给极低延迟目标加负优化。
仅在"空口拥塞被实证为主因"或 WAN 成为真实场景时重开；届时按"边缘编解码、核心 PCM"方案做（JB 不动），
重采样许可是前置范围决策。

## 15. 服务端重传（ARQ）不做

NACK 打来回（约 2–5ms）在 JB 水位内技术上塞得进，内存也只要几十 KB，但治的是随机丢包，
而 WiFi 真实死因是拥塞（重传火上浇油），且广播 + 多 client 有 NACK 风暴风险。顺序是：
先看计数器确认真有随机丢包 → 先试第 16 条奇偶包 → 还不够且确认非拥塞 → 才轮到 ARQ。

## 16. 奇偶包冗余（XOR parity，待讨论）

4 包带 1 校验包思路：无往返、无风暴、恒定约 25% 开销，孤立丢包全恢复，比 ARQ 配广播。
未做：先确认丢包真实存在且是随机的（见第 15 条顺序），再定分组大小与开销比。

## 17. 保留项（非问题，防后人"顺手"删改）

- `PlaybackManager::restart()` 无生产调用者：测试覆盖的挂载点，删了省不下什么，留给将来 GUI 重连。
- `AudioRoute{Application, nullopt}` 构造即静默等价跟随系统：三处边界（C API / 双 CLI）均拒绝空 id，
  构造不出来；`follows_system()` 访问器语义不闭合但零调用，动它不如不动。
- S_FALSE 丢弃 `closest_match`（playback）与 capture 侧 `!= S_OK` 拒绝：差异是刻意的（播放端引擎转换
  不改变会话语义，采集端重采样违反格式不可变），不要"顺手统一"。
- 回调超额返回帧 → `InvalidArgument` → `Degraded`：`JitterBuffer::pull` 受 `output.size()/frame_bytes_`
  限死，构造上不可达；响亮失败好过静默 clamp。
- 跟随系统 + 无关设备到达不重开、默认已跟上不再切：`UnrelatedDeviceArrival…` /
  `DefaultFollowAndSetChangeDoNotDoubleSwitch` 锁定，轮询 2Hz 下的必要约束。

## 18. ~~`--rpc-ip` pin 具体地址时不校验"控制面可达族 ⊆ 数据面服务族"~~（已关闭：启动期拒绝 + 逐族 warn/debug）

原问题：`--rpc-ip ::1 --udp-ipv6 none` 会正常启动，但控制面只对 v6 可达、数据面只服务 v4 ⇒ 所有客户端
都在 Connect 期被拒（`FAILED_PRECONDITION`），服务端 100% 不可用。反向组合（`--rpc-ip 127.0.0.1` + 默认
两族）则是 v6 数据面 socket 绑上了却没有任何 v6 客户端能连上控制面，且一行告警都没有。根因是
`network_design.md` 曾称该关系"由构造成立"——那只在通配绑定时成立，pin 具体地址时可达族会收窄。

已实施：`runtime::control_plane_reachable_families()`（通配 = 两族，具体 = 该族）+ 
`runtime::check_control_plane_coverage()`，在 `ServerRuntime::start()` 里判两次：

- **bind 之前**用**请求的**服务族：`可达 ∩ 服务 == ∅` ⇒ 拒绝启动（不碰任何 socket，实测该组合此前能起来）；
- **bind 之后**用**实际**绑定结果（含降级）复核：有可达却未被服务的族 ⇒ warn；有被服务却不可达的族
  （绑上却没人连得上的空转 socket）⇒ debug。

校验落在 Core（server 只有 CLI 一个前端，但 Core 才是公共入口，不依赖 CLI 是否拦过）。
测试：`ServerRuntimeConfigTest.ControlPlaneReachableFamiliesFollowTheBindAddress` /
`ControlPlaneCoverageSeparatesUnservedFromIdle` / `RejectsStartWhenNoReachableFamilyIsServed`。

## 19. ~~收包健康告警是"两族合计"口径，单族静默不告警~~（已关闭：闸门改逐族）

原问题：`check_udp_receive_health` 的 `silent_ticks` 以两族合计的 `heartbeat_received` 是否冻结为闸门，
于是 v4 在滴答时"v6 收包路径已死"永远不告警；`server_diagnostics_snapshot.h` 里"逐族由
check_udp_receive_health 判定"的注释与实现不符，INV-6（诊断逐族并行）在**自动告警**这一层没兑现。

已实施：

- `UdpServer` 的 heartbeat 计数按族拆分（`heartbeat_received(family)` / `heartbeat_rejected(family)`；
  合计与分项在同一处一起加，恒满足 合计 == v4 + v6），诊断行在两族都服务时多渲染 `v4hb` / `v6hb`；
- `SessionManager::create_session(expected_family)` 记下 Connect 期观测到的 peer 族，并新增
  `pending_family_counts()`：**还没收到首包**的 session 也按期望族计入"有人在等"——这是旧口径完全
  看不见的那批人（控制面连上了、UDP 打不通）；
- `check_udp_receive_health` 改成逐族闸门（v4 / v6 / 族未定 三桶），告警消息点名族并带上
  `rx(v4)`/`rx(v6)` 与 `waiting(v4)`/`waiting(v6)`。

残余（已知且可接受）：peer 族解析失败的 session（fail-open 路径）只能归入"族未定"桶，它只在
**没有别的族在等**时单独告警——否则会与 v4/v6 的告警同拍刷屏（那两行已把等待数一并打印）。

## 20. `noexcept` 边界内仍可能抛异常（OOM ⇒ terminate 而非 BackendFailed）

`WasapiAudioPlayback::start()` / `WasapiAudioCapture::start()` 声明为 `noexcept`（`audio_playback.h`
的纯虚接口要求），但函数体内的 `std::make_shared<StreamStartState>()`、两个 manager 的
`CallbackBundle` 拷贝、`std::optional<std::string>` 拷贝都会分配。分配失败抛 `std::bad_alloc`
⇒ 直接 `std::terminate`。

同理，RT 回调签名是 `noexcept` 的 `MoveOnlyFunction`，日志后端在极端情况下抛异常也会打穿。

接受理由：这些是 OOM / 日志后端故障路径，发生时进程本来也活不下去；`noexcept` 是接口契约
（回调不得抛、析构不得抛），改成可抛会把异常传播义务推给每一个调用点，收益远小于复杂度。
重开条件：若要在嵌入式 / 内存受限环境跑，或要求"任何失败都以错误码收敛"。

## 21. AAudio 析构不等待"迟到 error callback 窗口"

`AAudioAudioPlayback::~AAudioAudioPlayback()` 只有 `stop()`，而 `stop()` 结束后成员（含
`retired_slot_`、`event_callback_`）立即析构，没有任何"等迟到回调窗口过去"的机制。设计文档
（`aaudio_backend_design.md` §"退役槽位"）的前提恰恰是"AAudio error callback 不与 close() 同步，
close 返回后仍可能有一次迟到投递"——该前提在**对象存活期内**由 `retired_slot_` 兑现，但析构
路径没有兑现。

暴露面（复核后修正）：不是"仅进程退出那一刻"。Android 侧每次 `connect()` 都先
`releaseClient()`（`nativeDestroy`）再建新句柄，`AquaClient.connect()` 每次 `nativeCreate`
都会新建 `ClientRuntime` → 新建 `PlaybackManager` → **新建 `AAudioAudioPlayback`**，自动重连
走同一路径 ⇒ 析构每个会话发生一次。

实际风险仍然低：NDK 文档明确 error callback **不会因为应用自己的 stop/close 而触发**，要命中
需要一个"close 恰好与真实设备断连竞态"的窄窗口。已复核并**排除**的一个误报：`stop()` 的早退
守卫 `!running_ && stream_ == nullptr` 是正确的——"半死"态（`report_fatal_once` 置
`running_=false` 但不 close）下 `stream_ != nullptr`，不会早退，不会泄漏流。

重开条件：实机抓到退出/重连时的崩溃或 ASan 报告。观察方式应为**触发自动重连**
（播放中断开服务端），而不是只测"连接→播放→退出"。

## 22. Android 前台服务在后台重连时会再次请求启动；且从不降级

- `connect()` 无条件回调 `onConnectRequested`（`AquaController.kt`），该回调里
  `ContextCompat.startForegroundService`。App 退到后台后自动重连会再次请求启动 FGS
  （`targetSdk = 37`，Android 12+ 后台启动 FGS 受限）。
  缓解：服务在 Activity `onCreate` 就已启动，通常命中"已有 FGS"豁免。**未实机证实**，
  需测：切后台 → 停服务端 → 观察是否 FC / 能否恢复。
- `mediaPlayback` FGS 只在 `AquaService.onDestroy` 里 `stopForeground`；会话停止
  （`isRunning=false`）时不降级、不 `stopSelf`，未连接时通知常驻。
- 与 §13（固定 3s 退避）是两件事：那条讲退避策略，这条讲 FGS 合法性 / 类型前提。

接受理由：先实机确认再动，避免为未证实的问题改代码。重开条件：出现 FC 或系统回收。

## 23. server 侧 `UdpTransport` 与其 `State` 存在 shared_ptr 环

`UdpServer::start()` 装的收包回调（`udp_server.cpp`）按值捕获 `shared_ptr<UdpTransport>`
（heartbeat ACK 必须从收到包的同一条 socket 回去），而该回调被存进**这条 transport 自己**的
`state->handler`（`udp_transport.cpp`），transport 又持有 `state_` ⇒ 闭环。

唯一断点是 `close_state()` 里的 `state->handler = {}`，而 `UdpTransport::stop()` 只把关闭任务
**post 到 strand**，`server_main.cpp` 每条关闭路径都是 `server->stop(); ioc.stop();` 连写 ⇒
post 的任务基本永不执行 ⇒ 环不自破，`stop()` 注释里"最终由 State 析构释放句柄"因此自相矛盾。

当前影响：进程即将退出，泄漏由 OS 回收 ⇒ 实际无害。客户端 `UdpClient` 不捕获 transport，**无此环**。
重开条件：出现 server 重启、或 `ServerRuntime` 被嵌进长寿进程。
注意：改法（回调改捕获 `weak_ptr`）会改变 ACK 回包路径（`send_to` 前需 lock，可能失败），
属关闭期语义改动，需独立评审。

## 24. `extend_rtp_sequence` 在"扩展序列仍在首个周期"时的保守误判

见 `network_frame.h` 中该函数的完整注释。要点：单凭 16 位 raw 无法区分"大幅前跳"与"倒退超半窗
的迟到包"（RFC 3550 附录 A 本身的歧义），`base < 0x10000` 时周期数信息不存在，实现选择**按前跳
处理**——把真正的迟到包误判成大 gap，代价只是诊断计数虚高一次；反向误判会丢真实音频，代价不对称。

现实可达性需要"扩展序列尚在首个 65536 周期内 + 收到倒退超 32768 个包的包"，局域网不可达。
已知代价：入会初期可能各出现一次虚假的 gap 计数（`udp_client` 与 `jitter_estimator` 各一对），
JB 按 sanity 拒绝陈旧包，放音不受影响。测试 `network_frame_test.cpp` 锁定该行为。

**不要"顺手修"**：任何修改都会在信息不足时选错方向。排障读数时对照本条。

## 25. 服务端每族一条共享发送队列：N 个客户端时余量被摊薄，且 ACK 与音频共用

`UdpTransport::state_->send_queue` 是**每族一条**（server 侧 v4/v6 各一条），
`UdpServer::broadcast` 对所有 session 循环调`send_to_shared` 塞进同一条队列，
上限 `UDP_MAX_QUEUED_DATAGRAMS = 64`，满了 drop-oldest（`udp_transport.cpp`）。

| 同族客户端数 | 每客户端余量 | 等效音频时间 |
|---|---|---|
| 2（当前实测规模） | 32 包 | ~117ms |
| 10 | 6.4 包 | ~23ms |
| 30 | 2.1 包 | ~7.8ms |
| 64 | 1 包 | ~3.6ms |

一个停摆的客户端（合盖笔记本、休眠手机、停止 ACK 的 Wi-Fi 客户端）会把该族的 64 格
填满并开始挤掉**所有人**的音频。服务端一次 35ms 的调度停顿（正是
`JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS` 注释引用的实测常态）在 10 客户端时就足以触发。

更糟的一层：**heartbeat ACK 走同一条队列**（`udp_server.cpp`的 ACK 回包），所以音频压力下
ACK 会被同一个 drop-oldest 丢掉——而丢 ACK 正是客户端 liveness 的判据。数据面与控制面
被一个 64 格缓冲区耦合。

**当前影响：实测规模（1 笔记本 + 1 手机）无问题**，余量 32 包 ≈ 117ms，远大于任何瞬时抖动。
未做改造的原因：(1) 触发需要高客户端数，而本产品定位是局域网小规模；
(2) 正确改法是 per-destination 队列 + `EWOULDBLOCK` 时只丢那个目的地 + 控制流量单独队列，
属发送泵的结构性改动，当前无测试手段验证（没有压测台），盲改的风险高于收益。

重开条件：出现 ≥8 客户端，或实测到 `tx_dropped` / `heartbeat_ack_misses` 随客户端数上升。
届时优先级最高的是把 ACK 从音频队列里拆出来（改动最小、解耦最直接）。

## 26. auto-F 不与采集周期对齐 ⇒ packetizer 永久残留 + 冲发不均匀

`resolve_effective_frame_count`（`server_runtime.cpp`）取 `min(MTU 预算, 5ms 时长上限)`，
**从不参考采集周期**，而 F 必须在 `ServerRuntime` 的成员初始化列表里就确定（那时采集流
尚未打开）。WASAPI 共享引擎 loopback 固定 480 帧 = 10ms（`InitializeSharedAudioStream`
不接受 `LOOPBACK` flag，且被判非法的 flag 会黏在对象上，所以连试都不能试）。

于是默认格式（stereo F32，48kHz）F=175 时 `480/175 = 2.743` ⇒ `AudioPacketizer` 成为
**永久的 0..F-1 帧累加器**，残留均值 ≈87 帧 ≈ **1.82ms** 平均 / 3.63ms 最坏；且每次采集
回调冲发 2 或 3 个包（不均匀），dispatcher 的 pacing 就是为了抹平这个。

**为什么没有直接改**：
- 想让 F 整除采集周期，需要服务端在选 F 之前就知道采集周期。但 `AudioDeviceManager` 只暴露
  `default_format`，**没有周期查询**；而 loopback 的引擎周期在共享模式下必须 `Initialize`
  （即打开流）才能拿到——那是 F 定下来之后才发生的事。
- 另一侧（播放端 callback 尺寸 `cb`）属于**客户端**设备，服务端更无从得知，而
  `floor_latency(F) = (ceil(cb/F)+1)×F` 恰恰主要由 cb 决定。
- 实测两个候选方向相反：F=120 让冲发均匀（`480/120=4`）且把 floor 从 10.94ms 降到 10.0ms；
  F=160 也均匀但 floor 升到 13.3ms。**没有采集周期就选不出对的那个。**

结论：这是一个**需要更多信息的参数决策**，不是能靠改公式解决的 bug。CLI 已有
`--audio-packet-frames` 让高级用户按自己的设备指定；等有实测数据（各设备的采集周期 × 播放
cb）再把它变成一个正式的预设（"低延迟/均衡/吞吐"三档）才有意义。重开条件：拿到 ≥3 台设备的
采集周期与播放 cb 实测值。

## 27. 到达时间戳取自用户态而非内核

`udp_client.cpp` 的 arrival observer 用 `steady_clock::now()`，且是在
`async_receive_from` 的完成处理里、**strand 派发之后**才取。于是该 strand 上的任何调度抖动
都被直接注入 inter-arrival 统计 → 抬高 JitterEstimator 的 J → TargetController 抬 target →
稳态延迟上升。这是自伤式噪声：本项目花了很大力气在 stall/尾部项上做"打折"，却让测量本身
被派发路径污染。

正确做法是 `SO_TIMESTAMPNS`（内核到达时刻）。未做原因：需要 per-platform 的 `recvmsg` 分支
（Windows 与 POSIX 的 cmsg 解析不同），且当前只在 Windows 编译，收益要在真机上用
`estimator_jitter_ms` / `target` 的实测曲线才能确认值不值这个复杂度。

重开条件：真机实测发现 target 在空闲机器上仍明显高于几何地板（≈3 槽）。

## 28. 以"槽"计量、但所建模现象以时间计量的常量

以下常量的单位是槽，但它们约束的现象（采集周期突发深度）是时间量，因此**只在
`F=175 @48kHz` 这一个格式上正确**：

- `DISPATCH_PACING_CATCHUP_DEPTH_SLOTS`(8)：8 槽 = 29ms @F=175，但 = 120ms @F=48、
  = 3.6ms @F=911。
- `DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS`(48)：175ms @F=175，= 43.7ms @192kHz。
- `MIN_AUDIO_QUEUE_CAPACITY_SLOTS`(9)、`UDP_MAX_QUEUED_DATAGRAMS`(64)（后者见 §25）。

根因是 auto-F 随格式漂移，而这些值在构造期按 constexpr 槽数固定。正确修法是改成**毫秒**
并在解析出`frame_count`/`sample_rate` 之后换算成槽——但那会让
`runtime_config.h` 的 constexpr 变成 `ServerRuntime` 的运行期解析结果，是一个真实的 API 变更。

重开条件：出现非 48kHz/非 F32 的实际部署格式，或实测到 pacing 在某格式下失效。

## 29. liveness 检测偏慢，且没有 transport 层快路径

三处检测窗口（客户端被杀 / 拔网线 / 服务端被杀）目前是 **5.0~5.2s**（UDP 侧）/ **5.0~9.0s**（gRPC 侧），对局域网仍偏保守：

- UDP heartbeat：1s × 5 = 5s，再加 `SESSION_REAP_INTERVAL`（200ms）的扫描周期 ⇒ 5.0~5.2s。
  注意 reaper 走自己的 timer，与 `poll_control` 的 500ms tick 无关（曾误记为 6.5s）。
- gRPC keepalive：周期是 `interval + RPC`（最坏挂满 800ms deadline）⇒ 5.0~9.0s。
  **注意这不是误杀**（见 `udp_config.h` 的 `SESSION_TIMEOUT` 推导注释：服务端摘 session 的
  前提是"5s 内没成功处理任何 Keepalive"，此时客户端必然也独立判死，两边只是检测时刻不同）。

没有任何快路径：`grpc_client.cpp` 故意不调任何传输层参数（GOAWAY 顾虑是对的），但
**connectivity state 监听是纯增量的**——它只能让客户端**更早**发现，不带 GOAWAY 死亡语义。
订阅 `WatchConnectivityState`、把 READY→TRANSIENT_FAILURE 当即时 `TransportDead`，可以把
"服务端被杀"从 ~5.5s 降到 ~50ms。

另外 `HEARTBEAT_INTERVAL = 1000ms` 的理由（"NAT 锥形映射 30s+ 超时"）是从公网威胁模型搬来的，
对纯局域网产品偏保守；250ms 仍有 120倍余量，检测可以快 4 倍。

未做原因：检测慢不是正确性问题，而 connectivity-state 接入需要新的失败路径与测试（当前无
故障注入台）。重开条件：实测中"断开服务端 → 客户端仍播 5s 以上"被用户当成故障。

## 30. 发送线程没有提优先级

`AudioNetworkDispatcher::worker_` 与各 transport strand 全是默认优先级（全仓无
`SetThreadPriority` / `SCHED_FIFO`）。在忙的宿主上这是 **pacing 抖动的最大来源**，而 pacing
抖动会被对端测成 jitter → 抬 target。对一个以延迟为第一目标的产品这是最便宜的一笔，
但提优先级属于"用调度器配额换延迟"的取舍，且在桌面 OS 上可能被电源策略覆盖，收益需要实测。

重开条件：做了 §27（内核时间戳）后仍有明显 pacing 抖动，再评估提优先级是否还有边际收益。

## 31. Android 电池优化白名单：保持现状（明确不上架 Google Play）

`AndroidManifest.xml:8` 声明了 `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`，`SettingsScreen.kt:262-280`
用 `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` + `package:` URI **直接弹系统授权框**。
这套组合在 Google Play 属**受限用法**（需在 Play Console 申报豁免理由，只有"核心功能受电池优化
影响"才批）。

**决定：保持现状。** 产品侧明确**不上架 Google Play**（自用 / 局域网分发）⇒ 该政策不适用；
现在改掉只会白白牺牲 UX（一眼可见的"忽略电池优化"开关变成让用户去系统长列表里自己找本应用）。

若将来上架，二选一：
- 换成 `ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS` 并**删掉该权限**（UX 变差）；
- 或在 Play Console 走豁免申报（可能被拒，届时仍要回退前者）。

重开条件：决定上架 Google Play，或改用 App Bundle 分发。

## 32. Android 侧不做单元测试（JB 数值仍在演进期）

`app/src/test` 与 `app/src/androidTest` 目录**不存在**，而 `app/build.gradle.kts:107-112` 的
`testImplementation(libs.junit)` / `androidTestImplementation(...)` / `testInstrumentationRunner`
**都已声明** —— 是挂着没用的配置。

**决定：不做。** 理由：JB 的预设阶梯与各级常量**仍处于调参期**（2026-10 还在改 stall cap /
decay / 掩盖上限），此时把数值锁进单测，每调一次参都要同步改测试，净收益为负。

**已知代价（要承认，别当它不存在）**：

- **预设阶梯（11 档 × 5 条单调轴）目前没有任何机器守护。** 此前唯一的校验脚本
  `temp/check_preset_ladder.py` 已不在工作副本里（只在 `MEMORY.md` 与旧报告中被提及）⇒
  现在改预设不会被任何东西拦住。2026-10 出现过一次真实事故形状：core 的 `stallPeakCapSlots`
  8→12 会让"跟随 core"的 LAN 档**反超**显式写 10 的 WIFI 档，靠人工比对才发现。
- 同理，"容量 ↔ minTarget 的 1:2 绑定"也没有守护；`jbCapacity` 的**哨兵值 0**
  （= 跟随 core 默认 30 槽）曾被裸 `0` 直接比对、悄悄覆写成 4 槽（2026-10 已修）——
  这类 Bug 正是单测最擅长抓的。

重开条件：调参期结束（预设数值稳定一个发布周期），或再次出现"靠人工比对才发现"的阶梯事故。
届时的正确做法是**先把阶梯约束与 1:2 绑定抽成不依赖 Compose 的纯函数，再对纯函数写 JVM 单测**；
不要直接测 `AquaController`（34 个 `mutableStateOf`、依赖主线程与 Compose 快照，代价高得多）。
## 33. Android release 的宽签名上限：VerifyError（已修，沉淀为规则）

v0.4.0 开发期踩到一次：release 包**点"连接"就闪退**（退出码 `0x80000003`），
原文 `Verifier rejected class bc: mc bc.b() failed to verify: expected 16
argument registers, method signature has 17 or more`。用
`app/build/outputs/mapping/release/mapping.txt` 反混淆：`bc` = `AquaClient`、
`mc` = `AquaDiagnostics`。

**真正的病灶与连接逻辑无关**：`AquaDiagnostics` 当时是 **166 字段的 `data class`**，
Kotlin 生成的 `copy$default(...)` 有 160+ 个参数；`Long`/`Double` 在 DEX 里各占
**2 个寄存器槽**，总宽 270+，DEX/ART 表示不了 ⇒ 该类首次被加载/验证就失败。
先加的 `-keep` 只挡住了 R8 合并，所以崩溃点从"点连接"前移成"启动即崩"
—— 这是"挡板 ≠ 修复"的典型信号。

**修复**：`AquaDiagnostics` 改为 `LongArray` 存储 + 单参数构造 + 166 个只读 getter
（索引序 = `build_diagnostics()` 写入序，逐项对齐），并补 `@Immutable`
（Compose 的稳定性推导不认识数组，不声明会让所有接收它的 composable 失去跳过能力）。
keep 规则随之只剩 `AquaNative`（JNI 动态注册按类全名查找）与 `AquaClient`
（挡住 R8 垂直合并这条历史路径）。

**规则（下次别再交学费）**：

1. **不要把大结构体摊成构造参数**。字段超 ~40 个就改数组/分组持有；`data class`
   的 `copy$default` 会再叠一层 mask/marker，比构造函数更宽。
2. **按"寄存器槽"估算**，不按字段数：`Long`/`Double` = 2 槽，其余引用/标量 = 1 槽。
3. **`-keep` 只是挡板**：它改变的是 R8 的合并/内联决策。如果崩溃点只是"前移"，
   说明类的形状本身有问题。
4. **release 必须真机点一次"连接"**：`VerifyError` 发生在**首次加载该类**时，
   能启动到主页不代表没事（`AquaClient` 只在连接路径上构造）。
5. 排查顺序：`adb logcat -d -b crash` → 找 `VerifyError` 的类名 → `mapping.txt`
   反混淆 → 数该类的构造/`copy$default` 宽度。

这是**形态问题**，不是待办：不再有对应代码改动。若再次出现 `VerifyError`，按第 5 条走。
