# 捕获设备切换设计决议

本文冻结服务端捕获设备切换（capture 流切换）的架构。讨论时间：2026-09-02；实施前
最终决议，实现不得偏离本文。共享原则继承自 `playback_switching_design.md` §2（两侧
独立成文，不建 umbrella 文档；两文档实现节奏不同：playback 侧 Android/JNI/Kotlin/UI
已排期，capture 侧当前仅 WASAPI/CLI）。关联文档：`buffer_design.md`（client JB 饥饿
路径，本设计的韧性承担者）、`aaudio_backend_design.md`（capture 接口预留）。

## 1. 共享原则（与 playback 侧对称）

```
Session alive        —— 会话存活：restart 不触碰 gRPC/UDP/session
Format immutable     —— 格式不可变：restart 必须保持会话格式，无转码无协商
Endpoint replaceable —— 端点可替换：capture 流生命周期与会话生命周期独立
Timeline continuous  —— 时间线连续：seq/timestamp 不因 restart 重置
```

两侧对称式：

| 侧      | 对象            | restart 含义              | 韧性承担者                        |
| ------ | ------------- | ----------------------- | ---------------------------- |
| Client | AudioPlayback | 替换消费者（replace consumer） | 本侧 JB（水位机制吸收切换间隙）            |
| Server | AudioCapture  | 替换生产者（replace producer） | **对岸 client JB**（饥饿路径吸收发包间隙） |

Server 没有 JB，也不需要新增任何缓冲机制：capture restart 期间 packetizer 不产帧，
seq 不跳号（seq 由 packetizer 分配，与 capture 生命周期无关）；client 感知为一次普通
网络抖动（低水位 Fill / 静音 Hold / 恢复后继续），协议零改动。

## 2. 现状与病灶

现状（`server_runtime.h:160`）：捕获设备在**构造期一次性 resolve 并冻结**（注释明确
"Device changes require stop/restart"）；运行期设备错误（拔出/失效）经
`on_capture_event → Degraded`，CLI control timer `Degraded → stop()`——与 client 侧
supervision 同一把"一刀切"的刀，设备故障误杀整个 server 会话。

已有地基（无需新建）：

- CLI：`--capture loopback|input` + `--device-id`（省略 = 该方向系统默认）

- 路由模型：`AudioCaptureSource` + `AudioCaptureConfig.device`（nullopt 即 FollowSystem）

- WASAPI：`DEVICE_INVALIDATED → DeviceDisconnected` 映射、构造期格式校验、
  `IMMNotificationClient` 设备通知

- 本设计的缺口不是"设备模型"，而是"**生命周期管理**"：把构造期 device binding 升级
  为运行期可重建 endpoint。

## 3. 架构

```
ServerRuntime
 ├── gRPC / UDP / SessionManager / Packetizer / FrameQueue / Dispatcher ── 切换时纹丝不动
 └── CaptureManager（私有内部成员，对称 client PlaybackManager；暂不独立 .cpp）
       └── restart_capture(target)
              └── AudioCapture::start(source, device, 会话格式 + F)
                     ├── WASAPI    (endpoint 重建；格式校验路径已有)
                     ├── AAudio    (capture 未实现；落地时契约已就位)
                     ├── PipeWire  (未来)
                     └── CoreAudio (未来)
```

职责边界：CaptureManager 只管 capture 流生命周期 / restart 事务 / 回滚（机制）；
**何时** restart 由外部决策者驱动（策略，见 §6）。CaptureManager 不碰 packetizer /
network / session——它们持有 seq 与会话状态。

## 4. 路由模型（复用现有，不新增枚举）

保留 `(source, optional<AudioDeviceId>)`，不引入 CaptureMode/CaptureTarget：

| CLI 输入                            | 路由语义                                 |
| --------------------------------- | ------------------------------------ |
| `--capture input`（无 device-id）    | 跟随系统默认 **INPUT** 设备                  |
| `--capture loopback`（无 device-id） | 跟随系统默认 **OUTPUT** 设备的混音              |
| `--capture ... --device-id X`     | 优先 X（PreferredDevice 语义），不可用按 §5 链降级 |

- 无 `PreferCurrent`（server 无交互界面，无"保持当前"的用户语义）

- `source`（input ↔ loopback）**运行期不可改**——方向是配置级决策；且两方向的设备
  世界不同，绝不混向解析

- loopback 的 fallback 目标在 OUTPUT 方向 resolve；input 在 INPUT 方向

## 5. restart 事务链

所有切换场景（设备拔出 / 默认设备变化）收敛到同一个算法，由 CaptureManager 在控制
线程执行：

```text
restart_capture(target):                # target = nullopt(跟随系统) | device_id
    CaptureSwitchState = Switching
    捕获 previous_active_device         # 来自上次成功 resolve 的结果
    stop 旧 capture（同步 join capture 线程）   # 保证 packetizer 生产者唯一

    candidates = 去重([
        target_device,                  # 目标设备（跟随系统时为 nullopt）
        previous_active_device,         # PreferredDevice 失败的回滚项
        system_default(按 source 方向),  # nullopt 兜底项
    ])

    for c in candidates:
        if start(c, 会话格式, F) 成功:   # 格式校验复用现有 start 路径逻辑
            更新 active_device；CaptureSwitchState = Running
            上报 switch_result; return
    CaptureSwitchState = Fatal          # 链耗尽 = 格式不兼容
```

- 链固定三层，不做全设备遍历（共享原则的直接推论）

- `start()` 成功 ⇒ 实际流满足会话格式（encoding + channels 严格相等；采样率按平台
  例外，与 playback 侧 invariant 一致），否则 `FormatUnsupported`

- **Fatal 的语义**：Capture Fatal 意味着 server 无法提供所请求的音频源，因此会话
  终止（server 不是录音服务器，无 capture 的会话无意义）。timer 观察到 Fatal →
  `stop()`。`Running + Fatal` 只在 timer tick（500ms）内短暂共存。

场景走查：

| 场景                     | 路由                   | 链展开              | 结果                     |
| ---------------------- | -------------------- | ---------------- | ---------------------- |
| 默认输出设备变化（拔耳机）          | 跟随系统(loopback)       | \[新默认 OUTPUT]    | 重开跟随新默认；client 感知一次短抖动 |
| USB 麦克风拔掉              | 跟随系统(input)          | \[新默认 INPUT]     | 同上                     |
| 指定 DAC 被拔              | PreferredDevice(DAC) | \[DAC(跳过) → 新默认] | 落系统默认 + 日志"设备已断开"      |
| 新默认设备格式不兼容（如 16k mono） | 任意                   | 链耗尽              | Fatal → stop           |

**防抖**：所有自动 restart（错误驱动 + 默认变化驱动）10s 窗口内最多 3 次，超限按
链耗尽处理（防设备反复插拔风暴）。server 无手动切换，无窗口重置来源。

## 6. 触发机制：两条路径，全部轮询式

**路径 1 — 错误驱动**（设备拔出/失效）：

```text
WASAPI DEVICE_INVALIDATED → on_capture_event(DeviceDisconnected)
    → 不再置 Degraded；记录 switch_error，等待决策者驱动 restart
```

**路径 2 — 默认设备变化驱动**（跟随系统的主动跟随）：

```text
DeviceManager 新增 default_device_epoch(direction) 原子计数器
    （IMMNotificationClient::OnDefaultDeviceChanged 时递增）
→ 决策者轮询 epoch；跟随系统模式且 epoch 变化 → restart_capture(nullopt)
```

**决策者 = CLI control timer**（机制/策略分离，对称 client 的 Kotlin 层）：

```text
RuntimeState::Degraded（网络/分发原因）→ stop()        # 保留
capture switch_error                   → restart_capture(路由目标)
capture Fatal                          → stop()           # 唯一新终止条件
纯 capture 设备错误                    → 不再直接 stop()  # 不再误杀会话
default epoch 变化（跟随系统模式）     → restart_capture(nullopt)
```

未来若出现 GUI / Web 面板 server，由其自行实现决策者，core 契约不变。

**为什么轮询不回调**：与代码库 poll 诊断哲学一致；避免 COM 回调线程牵进 runtime
生命周期；epoch 计数器是值语义、天然线程安全、可进诊断快照。500ms tick 对设备
变化这类用户级事件无延迟敏感问题。

**触发源白名单**：只允许 `DeviceDisconnected` 与 `default_device_epoch 变化`。
**禁止**用 silence / low energy / no audio 等音频特征推断设备失效——WASAPI loopback
在无 render client 时静默并产合成静音是合法稳态（`wasapi_audio_capture.h:19`），
"活着但无声"≠"设备坏了"。

## 7. 状态与诊断

现有 `capture.state` 是**流级**状态（starved/silent 等），保留不动。新增**管理级**：

```text
capture_switch:
    state               # Inactive / Starting / Running / Switching / Fatal
    route               # follow_system(input) / follow_system(output) / preferred(device_id)
    active_device_id    # 实际 resolve 并成功打开的设备
    switch_result       # Switched / RolledBack / FellBackToSystem / Fatal + AudioError 原因
    default_epoch       # 各方向默认设备代数（供决策者/诊断观察）
```

`on_capture_event` 的 `Degraded` 迁移收窄为只由网络/分发层错误触发。

## 8. 线程与所有权边界

- **capture 线程同步 join 后才 start 新流**：任何时刻 packetizer 只有一个生产者
  （对称 client 的 JB SPSC 保护）

- stop/join 路径不得持有 capture 回调路径需要的锁；join 期间回调只做数据搬运与原子读

- **时间线不变式（server 特有核心约束）**：

  > **Capture restart preserves transport timeline** —— restart 允许 packet gap，
  > 但禁止 sequence reset、timestamp epoch reset、session recreation（除非链耗尽）。

  seq 与 timestamp 归 packetizer/dispatcher 所有，capture 生命周期 ≠ 流时间线。
  client 端将间隙当作网络抖动，JB 饥饿路径自动处理，恢复后无 reanchor。

## 9. 后端契约

`AudioCapture::start(source, device, 会话格式+F)`：成功 ⇒ 实际流满足会话格式
（encoding + channels 严格相等；采样率平台例外），否则 `FormatUnsupported`。

| 后端                   | 增量工作                                                                                                  |
| -------------------- | ----------------------------------------------------------------------------------------------------- |
| WASAPI               | `effective_capture_device_` 从构造期冻结改为运行期可变 + restart；epoch 计数器挂到已有 IMMNotificationClient；格式校验路径已有，零新协议 |
| AAudio               | capture 未实现（android\_roadmap 后续阶段）；落地时契约已就位                                                           |
| PipeWire / CoreAudio | 写新后端工作；契约就位后自动获得切换能力                                                                                  |

## 10. 范围裁剪（不做清单）

- **无运行时手动切换入口**：core 支持 `restart_capture()`，但当前 server application
  （CLI）不暴露运行时手动入口。手动路径 = stop 进程 + 换 `--device-id` 重启。未来
  GUI/Web server 可自行暴露，不推翻 core。

- 无 PreferCurrent；运行期不可改 source（input ↔ loopback）

- 不做 capture 侧 converter / 重采样 / 格式重协商

- 不做"指定设备拔掉后无限等待"（立即 fallback，对称 client 裁决 2）

- epoch 只跟踪默认设备变化，不跟踪设备增删列表（只有默认变化对切换有意义）

## 11. 实施阶段（全局排期）

在 client 侧计划（A-0/A-1/B，见 `playback_switching_design.md` §11）之后追加：

| 阶段          | 内容                                                                                                              | 前置                                           |
| ----------- | --------------------------------------------------------------------------------------------------------------- | -------------------------------------------- |
| **S1**      | CaptureManager + 错误驱动 restart + control timer 改造 + runtime 单测（mock capture：回滚 / 兜底 / 上限 / Fatal→stop / join 死锁） | A-1（复用 PlaybackManager 模式与测试方法）              |
| **S2**      | default\_device\_epoch + 跟随系统主动跟随                                                                               | S1                                           |
| C（client 侧） | Windows CLI playback 自动跟随                                                                                       | **与 S2 合并实施**（共享 epoch 基建与同一条 control timer） |

顺序理由：两侧共享同一个危险点——restart 生命周期正确性（stop/join/start/keep
session alive）；先在 client 侧验证模式，避免同时引入两个生命周期变化导致测试矩阵
爆炸。

**可选 S0（提前止血）**：若 Windows server 频繁因设备变化整体退出，可先行最小改动
——仅 `DeviceDisconnected → 同设备 restart 尝试`（无完整 fallback 链），正式
CaptureManager 仍按 S1 排期。是否需要由实际使用频率决定。

## 12. 实现风险排序

| 部分                        | 风险                    | 缓解                                 |
| ------------------------- | --------------------- | ---------------------------------- |
| capture thread join vs 回调 | **最高**（与 client 同源风险） | S1 专项测试：回调活跃时 restart 无死锁          |
| rollback correctness      | 中高                    | mock capture 全场景单测                 |
| 时间线连续性                    | 中                     | 断言 restart 前后 seq 单调不减、session 不重建 |
| epoch 轮询 / control timer  | 低                     | 值语义 + 现有 poll 模式                   |
| WASAPI 接入                 | 低                     | 格式校验路径已有                           |

## 13. 明确不做的事

- 任何 capture 侧格式转换（converter / resampler / channel mixer）

- gRPC 通知 client "capture 设备变化"（client 无感知，协议零改动）

- native 设备注册表 / umbrella 设计文档

- 静音/能量启发式设备检测

- 全设备遍历 fallback

- 运行时切换 capture source 方向


## 14. 实施修订记录（S1+S2 落地，2026-09-04）

实施时对本文的三处偏离与一处细化，记录备查（实现以此为准）：

1. **CaptureManager 独立成类**（偏离 §3"暂不独立 .cpp"）：实现为
   `include/aqua/audio/capture/capture_manager.h` + `src/audio/capture/capture_manager.cpp`，
   与 PlaybackManager 完全对称（含测试构造注入 mock 后端）。理由：S1 要求的
   mock 单测（回滚/兜底/上限/Fatal/join 死锁）需要独立注入点；私有内部类
   无法被测试触及。

2. **默认设备跟随用轮询替代 epoch**（偏离 §6 路径 2 的
   `default_device_epoch` + IMMNotificationClient 方案）：实现为
   `CaptureManager::tick()` 在 control tick（500ms）内轮询
   `default_device(direction)` 并与 active_device 比较，与 PlaybackManager::tick
   完全同构。理由：本文 §6 自己给出的轮询哲学（值语义、无 COM 回调线程
   生命周期问题）在轮询方案下同样成立，且代码库没有 IMMNotificationClient
   既有基建（§2 的"已有地基"清单此项与实际不符）；诊断快照不引入
   default_epoch 字段。触发源白名单语义不变。

3. **决策表落点在 ServerRuntime**（细化 §6"决策者 = CLI control timer"）：
   CLI timer 只做驱动（每 tick 调 `service_capture_switching()`）与终止
   （Fatal -> stop）；决策表本体（错误 pending -> restart_on_error /
   否则 tick / Fatal 上报）在 ServerRuntime 内。理由：路由推导需要
   runtime 内部状态（sticky 设备、路由模式、pending 标志），跨进程边界
   摊开策略反而割裂；机制/策略分离由"timer 驱动 vs runtime 执行"体现，
   对称 client 侧 supervision tick 的结构。

4. **格式钉死的实现**（细化 §5/§9）：首流成功后 `info().format` 钉进
   `active_config`（显式 format），后续候选 start 以显式格式请求，
   WASAPI 由 IsFormatSupported/Initialize 拒绝不兼容设备；manager 另做
   一层 post-start 复核（backend 未严格履约时该候选按 FormatUnsupported
   处理）。另：构造期 `effective_capture_device_` 保留仅用于格式探测
   （packetizer 几何），不再钉给运行期流——首流路由直接来自用户配置。
