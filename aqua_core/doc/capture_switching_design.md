# 捕获设备切换设计决议

本文描述服务端捕获设备切换（capture 流切换）的架构，实现不得偏离本文。共享原则继承自
`playback_switching_design.md` 第2节（两侧 独立成文，不建 umbrella 文档；两文档实现节奏不同：playback 侧 Android/JNI/Kotlin/UI
已排期，capture 侧当前仅 WASAPI/CLI）。关联文档：`buffer_design.md`（client JB 饥饿 路径，本设计的韧性承担者）、
`aaudio_backend_design.md`（capture 接口预留）。

## 1. 共享原则（与 playback 侧对称）

```
Session alive        —— 会话存活：restart 不触碰 gRPC/UDP/session
Format immutable     —— 格式不可变：restart 必须保持会话格式，无转码无协商
Endpoint replaceable —— 端点可替换：capture 流生命周期与会话生命周期独立
Timeline continuous  —— 时间线连续：seq/timestamp 不因 restart 重置
```

两侧对称式：

| 侧     | 对象          | restart 含义                   | 韧性承担者                                 |
|--------|---------------|--------------------------------|--------------------------------------------|
| Client | AudioPlayback | 替换消费者（replace consumer） | 本侧 JB（水位机制吸收切换间隙）            |
| Server | AudioCapture  | 替换生产者（replace producer） | **对岸 client JB**（饥饿路径吸收发包间隙） |

Server 没有 JB，也不需要新增任何缓冲机制：capture restart 期间 packetizer 不产帧， seq 不跳号（seq 由 packetizer 分配，与
capture 生命周期无关）；client 感知为一次普通 网络抖动（低水位 Fill / 静音 Hold / 恢复后继续），协议零改动。

## 2. 现状与病灶

现状（`server_runtime.h:192`）：捕获设备只在 **构造期 resolve 一次，且仅用于格式探测**
（`effective_capture_device_`，见 `modules/runtime.md`）；运行期路由来自 `config.capture.route`
（`AudioRoute`：跟随系统 / 钉住设备），restart 时按候选设备逐个重新 resolve。运行期设备错误（拔出/失效）经
`on_capture_event → Degraded`，CLI control timer `Degraded → stop()`——与 client 侧 supervision 同一把"一刀切"的刀，设备故障误杀整个
server 会话。

已有地基（无需新建）：

- CLI：`--capture loopback|input` + `--capture-device-id`（省略 = 该方向系统默认）

- 路由模型：`AudioCaptureSource` + `AudioCaptureConfig.route`（`AudioRoute`；`follow_system()` 即跟随系统）

- WASAPI：`DEVICE_INVALIDATED → DeviceDisconnected` 映射、构造期格式校验、 默认设备变化通知（由 `CaptureManager::tick()` 轮询
  `default_device` 实现，见 第6节 路径 2）

- 本设计的缺口不是"设备模型"，而是"**生命周期管理**"：把构造期 device binding 升级 为运行期可重建 endpoint。

## 3. 架构

```
ServerRuntime
 ├── gRPC / UDP / SessionManager / Packetizer / FrameQueue / Dispatcher ── 切换时纹丝不动
 └── CaptureManager（独立类，对称 client PlaybackManager：`../include/aqua/audio/capture/capture_manager.h` + `../src/audio/capture/capture_manager.cpp`，支持测试注入 mock 后端）
       └── restart_capture(target)
              └── AudioCapture::start(source, route, 会话格式 + F)
                     ├── WASAPI    (endpoint 重建；格式校验路径已有)
                     ├── AAudio    (capture 未实现；契约已就位)
                     ├── PipeWire  (未来)
                     └── CoreAudio (未来)
```

职责边界：CaptureManager 只管 capture 流生命周期 / restart 事务 / 回滚（机制）； **何时** restart 由外部决策者驱动（策略，见
第6节）。CaptureManager 不碰 packetizer / network / session——它们持有 seq 与会话状态。

## 4. 路由模型（与 playback 共用两根轴，不引入 capture 专属枚举）

保留 `(source, AudioRoute)`，不引入 CaptureMode/CaptureTarget。`AudioRoute`（给 backend 的请求）与 `RoutePolicy`
（manager 的决策状态）**与 client 侧是同一份类型**（`audio/devices/audio_route.h`、 `audio/devices/route_policy.h`；决议见
`playback_switching_design.md` 第4节 与 第16.1节）， capture 侧不新增枚举。两端唯一的差别是丢失动作，它现在是一个显式常量：

```cpp
CaptureManager::kLossAction = RouteLossAction::Fatal;   // client 侧恒为 FallbackToSystem
```

| CLI 输入                              | 路由语义                                                                 |
|---------------------------------------|--------------------------------------------------------------------------|
| `--capture input`（无 device-id）     | 跟随系统默认 **INPUT** 设备（`follow_system()`，意图归属 `None`）        |
| `--capture loopback`（无 device-id）  | 跟随系统默认 **OUTPUT** 设备的混音（同上）                               |
| `--capture ... --capture-device-id X` | 钉住 X（`pin(X)`，意图归属 `User`），不可用即 Fatal（stop），不降级到系统默认 |

- 无 `RouteIntentOwner::Application`（server 无交互界面，无"保持当前"的用户语义）

- **无 settle 预算**（client 侧 `playback_switching_design.md` 第16.3节 那套）：它是为 Android 的异步 AudioPolicy
  重路由而生的，而 Android capture 未实现，WASAPI 的设备失效也没有对应的窗口期。 加一套用不上的机制只是死重。

- `source`（input ↔ loopback） **运行期不可改**——方向是配置级决策；且两方向的设备 世界不同，绝不混向解析

- loopback 的 fallback 目标在 OUTPUT 方向 resolve；input 在 INPUT 方向

## 5. restart 事务链

所有切换场景（设备拔出 / 默认设备变化）收敛到同一个算法，由 CaptureManager 在控制 线程执行：

```text
restart_capture(target):                # target = nullopt(跟随系统) | device_id
    CaptureSwitchState = Switching
    active_generation = kNoStreamGeneration   # 先"退认"当前流：teardown 期间
                                              # 到达的事件在 manager 侧即被丢弃
    捕获 previous_active_device         # 来自上次成功 resolve 的结果
    stop 旧 capture（同步 join capture 线程）   # 保证 packetizer 生产者唯一

    candidates = 去重(
        跟随系统(owner=None): [target_device,             # nullopt（新系统默认）
                          previous_active_device,          # 回滚项
                          system_default(按 source 方向)]   # nullopt 兜底项
        钉住(owner=User):     [target_device]              # 无回滚/兜底
    )

    for c in candidates:
        if start(c, 会话格式, F) 成功:   # 格式校验复用现有 start 路径逻辑
            更新 active_device；active_generation = backend.generation()
            CaptureSwitchState = Running
            上报 switch_result; return
    CaptureSwitchState = Fatal          # 链耗尽（含钉住设备不可用）
```

- 链固定三层，不做全设备遍历（共享原则的直接推论）

- `start()` 成功 ⇒ 实际流满足会话格式（encoding + channels 严格相等；采样率同样严格相等（WASAPI capture 拒绝 `S_FALSE` 引擎重采样——采到被系统重采样过的音频会与会话格式不符，故视为不支持）），否则
  `FormatUnsupported`

- **Fatal 的语义**：Capture Fatal 意味着 server 无法提供所请求的音频源，因此会话 终止（server 不是录音服务器，无 capture
  的会话无意义）。timer 观察到 Fatal →
  `stop()`。`Running + Fatal` 只在 timer tick（500ms）内短暂共存。

场景走查：

| 场景                                | 路由                 | 链展开           | 结果                                  |
|-------------------------------------|----------------------|------------------|---------------------------------------|
| 默认输出设备变化（拔耳机）          | 跟随系统(loopback)   | \[新默认 OUTPUT] | 重开跟随新默认；client 感知一次短抖动 |
| USB 麦克风拔掉                      | 跟随系统(input)      | \[新默认 INPUT]  | 同上                                  |
| 指定 DAC 被拔                       | 钉住(User, DAC)      | \[DAC]           | Fatal → stop（不落系统默认）          |
| 新默认设备格式不兼容（如 16k mono） | 跟随系统             | 链耗尽           | Fatal → stop                          |

**防抖**：所有自动 restart（错误驱动 + 默认变化驱动）10s 窗口内最多 3 次，超限按 链耗尽处理（防设备反复插拔风暴）。server
无手动切换，无窗口重置来源。

## 6. 触发机制：三条路径，全部轮询式

**路径 1 — 错误驱动**（设备拔出/失效）：

```text
WASAPI DEVICE_INVALIDATED → on_capture_event(DeviceDisconnected)
    → 不再置 Degraded；记录 switch_error，等待决策者驱动 restart
```

**路径 2 — 默认设备变化驱动**（跟随系统的主动跟随）：

```text
CaptureManager::tick() 在 control tick（500ms）内轮询 default_device(direction)
    （与 PlaybackManager::tick 同构）
→ 与 active_device 比较；跟随系统模式且发生变化 → restart_capture(nullopt)
```

**路径 3 — 设备集合轮询驱动**（`tick()` 第 1 步，先于路径 2）：

```text
DeviceSetPoller 每 tick 枚举该 source 方向的设备集合，去抖后上报确认过的变化
→ on_device_set_changed(present)：正在采集的设备**不在**集合里 → restart_on_error()
   钉住归属 → 候选链只有 [intent] → 解析失败 → 链耗尽 → Fatal → server 退出
   跟随系统 → 重开当前默认
→ 正在采集的设备仍在 → 不动作（插拔的是别的设备）
```

路径 3 把"指定设备断了就直接退出"从*依赖 backend 投递流错误*变成**主动保证**：端点挂死而不报错时也能退出。
去抖（连续两次一致采样才上报）与"无信息"门控的理由见 `../playback_switching_design.md` 第17.3节——capture 侧共用
同一个 `DeviceSetPoller`。原始空列表是有效空集（`present=[]`）：全拔时活跃设备必不在集合内，直接 eager
restart → 钉住归属链耗尽 → `Fatal`；合成空 id 条目仍判无信息。capture **没有**自动切回：钉住丢失即 Fatal，不存在"降级后等原设备回来"的中间态
（第4节 `RouteLossAction`）。

诊断快照不引入 `default_epoch` 字段（代码库无 `IMMNotificationClient` 既有基建；轮询哲学与本文 第6节 自身一致）。

**决策者 = CLI control timer**（机制/策略分离，对称 client 的 Kotlin 层）：

```text
RuntimeState::Degraded（网络/分发原因）→ stop()        # 保留
capture switch_error                   → restart_capture(路由目标)
capture Fatal                          → stop()           # 唯一新终止条件
纯 capture 设备错误                    → 不再直接 stop()  # 不再误杀会话
default 设备变化（跟随系统模式）     → restart_capture(nullopt)
```

决策表本体（错误 pending → `restart_on_error` / 否则 tick / Fatal 上报）实现在 `ServerRuntime` 内——路由推导需要 runtime
内部状态（sticky 意图、路由策略、pending 标志），机制/策略分离由「timer 驱动 vs runtime 执行」体现，对称 client 侧 supervision
tick 结构。

未来若出现 GUI / Web 面板 server，由其自行实现决策者，core 契约不变。

**为什么轮询不回调**：与代码库 poll 诊断哲学一致；避免 COM 回调线程牵进 runtime 生命周期；轮询比较 `default_device`
的结果是值语义、天然线程安全、可进诊断快照。500ms tick 对设备 变化这类用户级事件无延迟敏感问题。

**触发源白名单**：只允许 `DeviceDisconnected` 与 default 设备变化（见路径 2）。 **禁止**用 silence / low energy / no audio
等音频特征推断设备失效——WASAPI loopback 在无 render client 时静默并产合成静音是合法稳态（`wasapi_audio_capture.h:21`），
"活着但无声"≠"设备坏了"。

**二次 restart 防护**：路径 1/2 都会在事务 `stop()` 阶段收到旧流临终 `DeviceDisconnected`（WASAPI 的
`SetEvent(error_event)`——现由 event 线程体内注册的 `stop_callback` 在 `request_stop()` 时执行——与 event 线程退出/join
之间仍有竞态，错误恰在事务窗口内回放），无防护会 latch pending 并对同一设备变化叠加第二次
`restart_on_error`（重复消耗 3/10s 预算、拉长静音）。三层防护：⓪ **事件归属过滤**（主过滤器）——事件带 stream
generation，`CaptureManager` 只放行等于 `active_generation_` 的事件；事务开头已先退认（置
`kNoStreamGeneration`），因此 teardown 期间的临终错误在 manager 内即被丢弃并计入 `stale_events_dropped()`， 根本不上到
runtime（契约见 `playback_switching_design.md` 第16.2节）；① `ServerRuntime::on_capture_event` 的 **Switching gate**
——manager 处于 Switching 时到达的错误一律视为旧流滞留错误，不置 pending、不迁移 Degraded（对称 client `on_playback_event`）。
generation 过滤是 **精确**判定，这道 gate 因此降为 **第二道防线**并刻意保留——它覆盖"新流已认领、state 尚未翻回
Running"的窄窗口；② `CaptureManager::tick()` 返回「是否执行了跟随事务」，事务成功后
`service_capture_switching` **吸收** pending 与锁存错误（对称 client `service_devices_changed`）——覆盖 gate
之外的残余竞态（错误在事务开始前一瞬、state 仍为 Running 时 latch）。残余窗口（事务内新流真实错误被 gate/吸收丢弃）由既有
`silent_death` 兜底（Running && !is_running → 下一 tick 恢复）。

## 7. 状态与诊断

现有 `capture.state` 是 **流级**状态（starved/silent 等），保留不动。新增 **管理级**：

```text
capture_switch:
    state               # Inactive / Starting / Running / Switching / Fatal
    route               # RouteIntentOwner 的标签：follow_system / preferred_device
                        # （route_intent_owner_label()；capture 侧不出现 prefer_current）
                        # User 归属时另有 dev 字段显示钉住的 device_id
    active_device_id    # 实际 resolve 并成功打开的设备
    switch_result       # Switched / RolledBack / FellBackToSystem / Fatal + AudioError 原因
```

`on_capture_event` 的 `Degraded` 迁移收窄为只由网络/分发层错误触发。

## 8. 线程与所有权边界

- **capture 线程同步 join 后才 start 新流**：任何时刻 packetizer 只有一个生产者 （对称 client 的 JB SPSC 保护）

- stop/join 路径不得持有 capture 回调路径需要的锁；join 期间回调只做数据搬运与原子读

- **时间线不变式（server 特有核心约束）**：

  > **Capture restart preserves transport timeline** —— restart 允许 packet gap，
  > 但禁止 sequence reset、timestamp epoch reset、session recreation（除非链耗尽）。

  seq 与 timestamp 归 packetizer/dispatcher 所有，capture 生命周期 ≠ 流时间线。 client 端将间隙当作网络抖动，JB
  饥饿路径自动处理，恢复后无 reanchor。

## 9. 后端契约

`AudioCapture::start(config, block_callback, event_callback)`——`config` 携带 `source` + `route`（`AudioRoute`）+
会话格式 + F：成功 ⇒ 实际流满足会话格式 （encoding + channels 严格相等；采样率同样严格相等（拒绝平台重采样）），否则
`FormatUnsupported`。backend 另需实现 `generation()`（ **纯虚**，对称 `AudioPlayback::generation()`），事件回调只允许上报
自己那条流的 generation，`AudioCaptureEventCallback` 即 `AudioStreamEventCallback` 的别名（见
`playback_switching_design.md` 第16.2节）。

首流成功后 `info().format` 钉进 `active_config`（显式 format），后续候选 `start` 以显式格式请求，WASAPI 由
`IsFormatSupported`/`Initialize` 拒绝不兼容设备；manager 另做一层 post-start 复核（backend 未严格履约时该候选按
`FormatUnsupported` 处理）。构造期 `effective_capture_device_` 保留仅用于格式探测（packetizer 几何），不再钉给运行期流——首流路由直接来自用户配置。

| 后端                 | 增量工作                                                                                                                                                                                            |
|----------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| WASAPI               | `effective_capture_device_` 从构造期冻结改为运行期可变 + restart；跟随系统由 `CaptureManager::tick()` 轮询 `default_device(direction)`（无 IMMNotificationClient 基建）；格式校验路径已有，零新协议 |
| AAudio               | capture 未实现；契约已就位                                                                                                                                       |
| PipeWire / CoreAudio | 写新后端工作；契约就位后自动获得切换能力                                                                                                                                                            |

## 10. 范围裁剪（不做清单）

- **无运行时手动切换入口**：core 支持 `restart_capture()`，但当前 server application （CLI）不暴露运行时手动入口。手动路径 =
  stop 进程 + 换 `--capture-device-id` 重启。未来 GUI/Web server 可自行暴露，不推翻 core。

- 无 `RouteIntentOwner::Application`；运行期不可改 source（input ↔ loopback）

- 不做 capture 侧 converter / 重采样 / 格式重协商

- 不做"指定设备拔掉后无限等待"：钉住的设备（`RouteIntentOwner::User`）不可用即 Fatal（stop）， 不降级到系统默认（区别于 client
  侧"永不主动静音"的 fallback——server 钉住 设备 = "只要这个设备"）。这份两端相反的取舍现在是显式的
  `RouteLossAction`（第4节），不再靠两个同名不同义的枚举值隐含。

- 跟随系统的**切换决策**只看默认设备变化（轮询 `default_device`），不因为某个无关设备插拔就重开流。
  设备增删列表本身是被跟踪的（第6节 路径 3 的 `DeviceSetPoller`），但 capture 侧只用它回答一个问题：
  **正在采集的设备还在不在**。别的设备来来去去与本会话无关——重开只会白白制造一次采集空档。

## 11. 实现状态

以下路径均已实现并由单测锁定（`testing.md` 第7节），模式与测试方法复用 playback 侧：
`CaptureManager` + 错误驱动 restart + control timer 改造 + runtime 单测（mock capture：回滚 / 兜底 /
上限 / Fatal→stop / join 死锁），`CaptureManager::tick()` 两步轮询（设备集合 + `default_device`
跟随系统主动跟随），Windows CLI playback 自动跟随（共享轮询基建与同一条 control timer）。

两侧共享同一个危险点——restart 生命周期正确性（stop/join/start/keep session alive），已由 client
侧先验证模式。

## 12. 风险与锁定测试

| 部分                                  | 风险                           | 锁定测试                                       |
|---------------------------------------|--------------------------------|------------------------------------------------|
| capture thread join vs 回调           | **最高**（与 client 同源风险） | 回调活跃时 restart 无死锁                      |
| rollback correctness                  | 中高                           | mock capture 全场景单测                        |
| 时间线连续性                          | 中                             | restart 前后 seq 单调不减、session 不重建（结构保证，评审项） |
| `default_device` 轮询 / control timer | 低                             | 值语义 + 现有 poll 模式                        |
| WASAPI 接入                           | 低                             | 格式校验路径已有                               |

## 13. 明确不做的事

- 任何 capture 侧格式转换（converter / resampler / channel mixer）

- gRPC 通知 client "capture 设备变化"（client 无感知，协议零改动）

- native 设备注册表 / umbrella 设计文档

- 静音/能量启发式设备检测

- 全设备遍历 fallback

- 运行时切换 capture source 方向
