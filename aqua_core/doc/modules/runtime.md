# 模块：Runtime

`ServerRuntime` 与 `ClientRuntime` 是两端的装配与生命周期边界：它们持有网络、会话、队列与音频组件，并把音频组件交给各自的
切换管理器（`CaptureManager` / `PlaybackManager`）托管。本文档只描述编排与职责，切换事务细节见
`capture_switching_design.md` / `playback_switching_design.md`，线程归属见 `threading_and_lifecycle.md`。

## 1. ServerRuntime

### 构造期

构造期只做 **不会失败**的解析与对象创建，失败一律留给 `start()` 报出：

```text
resolve capture device（按 source 方向）  -> effective_capture_device_     仅用于探测格式
resolve AudioFormat（用户指定 ?: 设备 mix format） -> effective_format_
resolve F（auto ?: 显式，受 MTU 预算约束）        -> effective_frame_count_
构造 SessionManager / UdpServer / AudioPacketizer / AudioFrameQueue / AudioNetworkDispatcher
```

`effective_capture_device_` 只服务于格式探测——packetizer 与队列的几何必须在 `start()` 之前确定。 **运行期不走这个字段**：
采集路由来自 `config.capture.route`（`AudioRoute`：`follow_system()` = 跟随系统，`pin(id)` = 指定设备），切换时由
`CaptureManager` 重新解析候选设备。

### start () 顺序

```text
Created -> Starting
  校验 format / F / queue slots / timeout / 端口 / 几何 / IP 合法性
  创建 ReapState（reaper strand + timer）
  创建 CaptureManager（内部持有平台 AudioCapture 后端）
  bind UDP 两族 + 启动逐族接收循环（失败语义见下）
  启动 dispatcher worker
  启动 capture（并校验 backend 实际格式 == effective_format_）
  构造 gRPC server（两个通告槽由"哪一族真的绑上了"决定）
  spawn gRPC worker 线程
Running
  在 strand 上挂 session reaper
```

任意步骤失败都回到 `stop_locked()`，按逆序清理已创建的资源；Runtime 不向调用者暴露"半启动"状态。

### 数据面绑定与通告槽

数据面是**逐族**的，配置解析 → 绑定 → 通告三者一一对应：

```text
--udp-ipv4 / --udp-ipv6（或 none）
  └─ 解析与校验：本族原生字面量；link-local 必须带 zone；
                 通告必须与绑定同族且为具体地址、不带 zone；绑定为 none 时不许配该族通告
       └─ udp_.bind(plan)：两条 socket 共用 udp_port
            └─ 失败语义：pin 的族失败 ⇒ 启动失败（响亮，不静默换地址）
                         默认通配的族失败 ⇒ 该族降级 + warn，另一族照常
                         任一族 receive 起不来 ⇒ 整体回滚（不留"绑上了但收不到"）
                 └─ UdpPlaneDescriptor：槽存在 ⟺ 该族绑定成功（INV-7）
```

槽的存在性是"控制面可达族 ⊆ 数据面服务族"的**构造性**保证：客户端按自己的族选槽，选不到就在 Connect 期失败。
服务端另有一道防御性准入（`ctx->peer()` 解析出的族未被服务 ⇒ `FAILED_PRECONDITION`；peer 族解析不出来时**不**拒绝，
宁可漏判）。设计推导见 `../network_design.md` §2–§3，socket 层细节见 `udp_transport.md`。

启动行会把控制面绑定、数据面端口、服务族集合与两个槽值一起打印 —— "三个绑定 + 两个槽"要一眼可见。

### 数据路径

```text
AudioCapture 回调（RT 线程）
  └─ on_capture_block -> packetizer_.push
        └─ frame_queue_.push -> dispatcher_.publish_from_realtime()
              └─ dispatcher worker: drain queue -> encode -> udp_.broadcast
```

### 运行期控制：service_capture_switching ()

由 CLI control timer 每 500ms 在 io_context 线程调用（经 `lifecycle_mutex_` 与 `start()` / `stop()` 串行化）。它是 capture
切换的决策表：

```text
capture 管理状态 == Fatal                 -> 返回 Fatal（CLI 据此 stop）
设备错误待处理（DeviceDisconnected）      -> CaptureManager::restart_on_error()
                                             成功后清零 last_audio_error_
否则                                       -> CaptureManager::tick()（返回是否执行了事务）
                                             （① 设备集合轮询：正在采集的设备消失即 restart；
                                              ② 跟随系统时轮询系统默认设备变化并跟随）
                                             事务成功 -> 吸收 pending + 清零错误，返回 Restarted
```

restart 事务（stop → join → start）在该调用内同步完成。期间 packetizer 没有生产者，client 侧感知为一次普通网络抖动。
错误驱动与默认跟随共用同一个 10s / 3 次的重试预算，超限即 Fatal。

### 运行期事件：on_capture_event ()

backend 事件回调运行在 backend 的 event 线程，只做标志置位，绝不执行 stop/start。事件 **先经 `CaptureManager` 的 generation
过滤**（主过滤器）：不属于当前流的事件计入 `stale_events_dropped()` 后丢弃，根本到不了这里。放行者的去向：

| 事件                   | 处理                                                                                  |
|------------------------|---------------------------------------------------------------------------------------|
| manager 处于 Switching | 旧流滞留错误（事务 stop 阶段投递）：丢弃，不置标志、不迁移 Degraded（防二次 restart）。 generation 过滤之后这道 gate 降为**第二道防线**，刻意保留：它覆盖"新流已认领、state 尚未翻回 Running"的窄窗口 |
| `DeviceDisconnected`   | 置 `capture_device_error_pending_`，等 control tick 触发切换；**不置 Degraded**       |
| 其它（后端内部错误等） | 置 `last_audio_error_`，Runtime 状态迁 `Degraded`（CLI 下一 tick 停止）               |

只用 `DeviceDisconnected` 作为切换触发源。静音、低能量、"长时间无音频"都不能用来推断设备故障——loopback 在没有 render client
时静默并产出合成静音是合法稳态。事务窗口内被丢弃的新流真实错误由 silent_death 兜底 （管理状态 Running 但 backend 已停止 ->
下一 tick 按设备错误恢复）。

## 2. ClientRuntime

### 启动顺序

```text
Created
  校验 client 配置
  创建 DeviceManager
  创建 PlaybackManager（内部持有平台 AudioPlayback 后端）
  gRPC 连接 + Connect RPC（响应里是两个通告槽：udp / udp6）
  按自己的族选槽 ⇒ 得到最终 UDP 目标（空槽 / 端口 0 / 跨族地址 = 硬失败）
  校验返回的格式 / F / payload 几何
  创建 JitterBuffer 与回放流水线
  配置 UDP remote（set_remote 按目标的族开 socket）
  启动 UDP 接收
  启动 heartbeat（握手节奏建连，建连后同一包型转 5s 续命节奏）
  启动 AudioPlayback（起步 pin 失败则回退系统默认重试一次，成功后
    adopt_user_intent(原 id) 补装 sticky 意图：auto_return 与诊断与运行期一致）
Running
```

`connect_result()` 锁内按值返回：`start()` 在 `lifecycle_mutex_` 下写，C API/JNI 轮询线程可在
`start()` 阻塞于 gRPC 时并发读（`take_diagnostics_snapshot` 同样持锁，对称）。

顺序的意义是：在回放回调开始之前，网络目标、格式、F 与 JitterBuffer 存储都已就绪。

`start()` 在任何副作用（设备/资源创建）之前先校验端点配置（`validate_client_endpoint`）：`rpc_ip` 必须是
**具体**的 IP 字面量（不通配）、非 v4-mapped、`rpc_port > 0`。这条守卫由 Core 提供 —— `rpc_ip` 同时是
sentinel 回落时数据面的拨号目标，而 C API / JNI 不经过 CLI parser（INV-8）。

选槽规则（客户端族在配置期就唯一确定 —— `--rpc-ip` 必须是具体 IP 字面量，不通配、不解析 DNS）：

```text
client_family = effective_family(--rpc-ip)
slot          = client_family == V4 ? resp.udp : resp.udp6
槽不存在 / port == 0 / 空地址   -> 硬失败（不静默回落到 gRPC 地址）
槽地址是通配 sentinel           -> 用 --rpc-ip 的字面量（族必然一致）
槽地址不是本族原生形态          -> 硬失败（两侧配置/版本不一致）
```

**不做候选回退**：槽是服务端按族给的，回退只会在错误的地方制造第二个权威。选中的槽以 info 级打印
（`client_family=` / `slot=` / `udp=` / `sentinel=`）—— UDP 被防火墙挡掉这类失败服务端判不出来，
"客户端实际在用哪个端点"是运维唯一的线索。C++ 侧 `ConnectResult` 另外暴露 `udp_family`（选中的族 =
客户端自己的族）与 `udp_address_from_sentinel`（地址是否来自 sentinel 回落）；C API 暴露的是**已解析的
最终拨号地址**（`advertised_udp_address`），如果需要族，从该地址推导即可。

失败时回到 `stop_locked()`，按逆序清理。

### 数据路径

UDP 接收回调在 transport strand 上构造 `AudioFrame{sequence, frame_count, pcm_view}` 并 `jb->push()`；JitterBuffer 复制
数据，不依赖接收缓冲区生命周期。回放回调只做 `JitterBuffer::pull(output)`。

### 运行期控制

CLI control timer 每 500ms 调用两个入口（同一控制线程串行）：

- `service_playback_recovery()`：设备错误标志或"静默死流"（管理状态 Running 但 backend 已停止）→ 走
  `PlaybackManager::restart_on_error()` 候选链；成功后清零错误通道。`restart_on_error()` 内部先给失败分类：**路由未稳定**
  （流刚起来就死）走独立的 settle 预算并按 200ms 节流，节流命中时 **不改任何状态**直接返回，正是靠这里的"静默死流"分支在下一个
  tick 重新驱动恢复（ioc 线程上不 sleep，UDP 心跳不被饿死）；其余走 10s/3 的设备丢失预算。
- `service_default_device_follow()`：转发到 `PlaybackManager::tick()`。名字叫 device_follow，实际转发的是**两步**轮询：
  ① 设备集合变化（`DeviceSetPoller` 去抖后交给 `on_devices_changed`，对**所有归属**生效——Windows 上这是钉住设备
  回归自动切回的唯一触发源）；② 跟随系统（`RouteIntentOwner::None`）时比较系统默认输出设备。第 ① 步动作过即返回，
  两步不会对同一次插拔各切一次。

此外 `notify_devices_changed(ids)` 可由任意线程调用（Android 的 Kotlin 回调线程即如此）：`ids` 是未过滤 sink
全集，事件 post 到 io_context，经 1s 合并窗口去抖后，由 `PlaybackManager::on_devices_changed()` 完成全部路由决策。
`on_devices_changed` 以 `switch_seq` 是否变化报告"真跑了事务"：节流空操作报 `false`（service 不吸收标志、不清错误通道），
预算耗尽 `Fatal` 报消费（service 靠 `Running` 守卫不会误清）。推送与轮询**共用这一份决策逻辑**；
Android 上 `DeviceSetPoller` 因 `enumerate()` 只给空 id 合成条目而恒判定「无信息」，因此两个生产者不会同时喂它。

## 3. Degraded

`Degraded` 表示"运行期出现了无法自愈的终止条件"，由 CLI control poll（500ms）观察并 stop。它是 **一次性终态**，没有回到
`Running` 的路径。

能进入 `Degraded` 的只有两类情况：

1. 非设备的后端/网络终止条件（capture 的非 `DeviceDisconnected` 错误、client 的非设备类错误）；
2. 切换链耗尽或重试预算超限（管理状态 Fatal）。

设备失效本身 **不再**把 runtime 打成 `Degraded`——它是可自愈的，由切换事务吸收。
