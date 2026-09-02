# 播放设备切换设计决议

本文冻结客户端播放设备切换（流切换）的架构。讨论时间：2026-09-02；实施前最终决议，
实现不得偏离本文。背景：Android 侧 AAudio 后端未实现设备层，流一旦建立无法切换到其他
输出设备（蓝牙耳机接入、USB DAC 插入等场景）。关联文档：`aaudio_backend_design.md`（§3
设备路由）、`buffer_design.md`（JitterBuffer 模型）。

## 1. 三个概念的分离

本设计的基础是把三个容易混淆的生命周期彻底拆开：

| 概念       | 含义     | 载体                                      | 切换设备时    |
| -------- | ------ | --------------------------------------- | -------- |
| Session  | 网络音频会话 | gRPC / UDP / JitterBuffer / session\_id | **纹丝不动** |
| Playback | 本地音频消费 | 一条 AudioPlayback 流（JB 的唯一消费者）           | 销毁重建     |
| Route    | 声音去哪里  | 路由模式 + 目标设备（connection 属性）              | 按事务链变更   |

Server 与网络协议对客户端的设备切换**零感知**。

## 2. 冻结原则

1. **Session format is immutable** —— 会话内音频格式不变。无协商、无转码、无 server 通知。
2. **Device switching is client-local** —— 设备切换只影响客户端 playback。
3. **Playback restart ≠ session restart** —— 播放与网络生命周期独立；JB、seq、playhead
   跨设备切换保持连续，不清空、不重新 pre-roll。
4. **Never stay silent voluntarily** —— 切换失败沿固定 fallback 链降级，只有格式不兼容
   （链耗尽）才终止音频。
5. **No professional routing** —— 不做 converter、动态格式协商、多输出图、全设备遍历。
   Aqua 停在消费级软件这一侧。
6. **不为形式统一制造桥接** —— Android 的设备发现/通知留在 Kotlin 层（`AudioManager`），
   不建 native 设备注册表。跨平台统一抽象的边界就是 `AudioPlaybackConfig.device`：
   谁产生 device id 不重要，各平台可以不同。

## 3. 状态模型

### 3.1 RuntimeState（不动）

`Created / Starting / Running / Degraded / Stopping / Stopped`，语义收窄为**网络会话生命**。
`Degraded` 从此只由网络原因触发。

### 3.2 PlaybackState（新增，平行维度）

```cpp
enum class PlaybackState {
    Inactive,   // 未启动（连接前 / 已停止）
    Starting,   // 首次启动中
    Running,    // 流在跑
    Switching,  // restart 事务进行中（旧流已停、新流未成）
    Fatal,      // fallback 链耗尽（格式不兼容）；runtime 终止前的最后状态
};
```

**Fatal 的定义必须明确**：它不是普通的 playback 失败，而是 fallback 链耗尽的终态。
supervision 观察到 `Fatal` 后 `stop()` 整个 runtime（§6），因此 `Running + Fatal` 只在
supervision tick（500ms）内短暂共存。日志中看到 `PlaybackState=Fatal` 即代表会话即将终止。

### 3.3 合法组合

| RuntimeState | PlaybackState | 含义                            |
| ------------ | ------------- | ----------------------------- |
| Running      | Running       | 正常播放                          |
| Running      | Switching     | 切换中（网络正常，UI 不应变红）             |
| Running      | Starting      | 首流建立中                         |
| Running      | Fatal         | 瞬态（≤500ms，supervision 将 stop） |
| 其他           | Inactive      | 未连接 / 已停止                     |

## 4. 路由模型

路由是 **connection 属性**，不持久化（设备 id 跨会话不稳定）；每次连接按设置起步。
唯一持久化的是"自动切换播放设备"开关。

```cpp
enum class PlaybackRouteMode {
    FollowSystem,       // 跟随系统默认（"自动切换"开）
    PreferCurrent,        // 保持当前实际设备（"自动切换"关；首流成功后钉住实际设备）
    PreferredDevice,    // 优先指定设备（用户手动选择；不可用时按 fallback 链降级）
};
```

> `PreferredDevice` 的语义是**优先**而非**固定**：设备不可用时按 §5 的链降级，不无限等待。
> 这是与"FIXED 永不偷偷切"的旧表述的有意决裂——永不主动静音优先。

UI 映射：

| 用户动作                | 路由模式                  |
| ------------------- | --------------------- |
| 设置"自动切换播放设备" ON（默认） | 连接时 `FollowSystem`    |
| 设置"自动切换播放设备" OFF    | 连接时 `PreferCurrent`     |
| 弹层选"跟随系统"           | `FollowSystem`        |
| 弹层选具体设备             | `PreferredDevice(id)` |

错误驱动 restart 的目标由当前模式推导：`FollowSystem → 系统默认(nullopt)`；
`PreferCurrent → 之前的实际设备 id`；`PreferredDevice(id) → id`。

## 5. 统一 restart 事务链

所有切换场景（手动选择 / 设备拔出 / 流断开错误）收敛到**同一个算法**，由
PlaybackManager（ClientRuntime 私有内部成员，见 §10）执行——错误驱动的恢复经
`on_playback_event` 即时 `asio::post` 派发到 ioc 线程就地执行（检测延迟 ~20ms，
不等待 supervision 的 500ms tick），手动切换经 C API 控制路径同步调用：

```text
set_playback_device(target):            # target 由路由模式推导或用户直接给出
    PlaybackState = Switching
    捕获 previous_active_device         # 来自 AudioStreamInfo 的实际设备回读
    stop 旧流（同步 join 回调线程）      # break-before-make，保证 JB 消费者唯一

    candidates = 去重([
        target_device,                  # 目标设备（FollowSystem 时为 nullopt）
        previous_active_device,         # 手动切换失败的回滚项
        system_default,                 # nullopt 兜底项
    ])

    for c in candidates:
        if start(c, 会话契约格式) 成功:
            更新路由模式与实际设备；PlaybackState = Running
            上报 switch_result（含降级原因，驱动 UI 横幅）; return
    PlaybackState = Fatal               # 链耗尽 = 格式不兼容
```

要点：

- **链固定三层**，不做全设备遍历（原则 5 的直接推论）。链条冗余重试无害（start 幂等）。

- `previous_active_device` 与 `system_default` 可能解析到同一物理设备，仅按 id 相等去重。

- **JB 不清空**：切换间隙 JB 水位上涨，deadline-high Drop 自动跳到最新；新流接上后
  play\_seq 连续。切换逻辑完全不碰 JitterBuffer。

场景走查：

| 场景                            | candidates 实际展开       | 结果                     |
| ----------------------------- | --------------------- | ---------------------- |
| 手动选 USB DAC，格式 OK             | \[DAC]                | 无感切换                   |
| 手动选 USB DAC，FormatUnsupported | \[DAC → 旧设备 → SYSTEM] | 回滚旧设备 + 横幅"切换失败"       |
| PreferredDevice 的 DAC 被拔      | \[DAC(跳过) → SYSTEM]   | 立即落 SYSTEM + 横幅"设备已断开" |
| FollowSystem 下系统 reroute 杀流   | \[SYSTEM]             | 重开即跟随新默认               |
| PreferCurrent 下流死亡              | \[旧设备 → SYSTEM]       | 旧设备还在则原地重开             |
| SCO/HFP 接入（16k mono 不兼容）      | 链耗尽                   | Fatal → stop           |

**防抖与重试上限**：错误驱动的自动 restart 在 10s 窗口内最多 3 次，超过按链耗尽处理
（防蓝牙连接风暴造成重启死循环）。用户显式选择不计数并重置窗口。Kotlin 侧设备事件
做 1s 合并窗口，最新目标胜出。

## 6. supervision 边界

`aqua_capi.cpp` 的 supervision tick 与 CLI control timer 同步改为：

```text
hello_failed                      → stop()     # 保留
RuntimeState::Degraded（网络原因） → stop()     # 保留
PlaybackState::Fatal              → stop()     # 唯一新终止条件
PlaybackState::Switching / 设备错误 → 不动作    # 不再误杀会话
```

## 7. 线程模型与 SPSC 保护

| 约束            | 方案                                                                                                                  |
| ------------- | ------------------------------------------------------------------------------------------------------------------- |
| JB SPSC 契约    | break-before-make：`stop()` 同步 join 旧回调线程后才 `start()` 新流，任何时刻 JB 只有一个消费者                                             |
| 事务线程（core）    | 错误恢复经 `on_playback_event` 即时 `asio::post` 到 ioc 就地执行（stop/join/start 阻塞 ~数十 ms，HELLO 定时器延迟一拍无害）；手动切换经 C API 控制路径同步调用                          |
| 控制串行化（core）   | restart 与 start/stop 同在 runtime 控制路径（C API 经 ioc 调度，与 supervision 同 strand）                                         |
| 控制串行化（Kotlin） | 所有 native 生命周期调用继续走 `lifecycleExecutor`；AudioDeviceCallback 在 binder 线程 → mainHandler → controller 决策 → executor 执行 |
| 死锁防护          | stop 路径不得持有回调路径需要的锁；stop/join 期间回调只做 JB pull 与原子读                                                                   |

**Phase A-0 必须包含专项测试**：`restart_playback while callback active`——回调正在
`JB.pop()` 时发起 stop/join，验证无死锁、无双重消费、seq 连续。

## 8. 后端契约

统一契约：`AudioPlayback::start(config)`，其中 `config.device = nullopt | id`，
`config.format` = 会话契约格式，**永不由后端改写**。

**Invariant**：`start()` 成功 ⇒ 后端以请求的 encoding + channels 消费数据；**采样率允许
平台透明 SRC**（AAudio 既有决议，`aaudio_backend_design.md` §1.2——系统内重采样不改变
JB 的时钟基准，不违反 session immutability）。后端不得偷偷改变 channels/encoding 后返回
成功；不支持即返回 `FormatUnsupported`。

| 后端        | device 传入                        | 实际设备验证                                                         | 格式不兼容来源                        | 增量工作                                         |
| --------- | -------------------------------- | -------------------------------------------------------------- | ------------------------------ | -------------------------------------------- |
| AAudio    | `"android:N"` → `setDeviceId(N)` | open 后 `getDeviceId()` 回读进 `AudioStreamInfo`（新增 device\_id 字段） | SCO/HFP（16k mono）              | `resolve()` 放行 `android:N` + builder 一行 + 回读 |
| WASAPI    | endpoint id（枚举已有）                | 激活的 endpoint 即所请求                                              | `IsFormatSupported` 预检失败（路径已有） | **零改动**（endpoint 重建天然就是 restart 语义）          |
| PipeWire  | `PW_KEY_TARGET_OBJECT`           | stream 状态机                                                     | 几乎不会（adapter 自动协商）             | 属于写新后端，契约已就位                                 |
| CoreAudio | HALOutput `CurrentDevice`        | 属性回读                                                           | 几乎不会（AudioConverter）           | 同上；坚持 break-before-make，不用 HAL 热切特权路径        |

## 9. 接口面

**C API**（thin，不含策略；**异步请求语义**——调用返回 ≠ 切换完成，结果经诊断观察）：

```c
// device_id == NULL 表示跟随系统（FollowSystem）
int aqua_client_set_playback_device(aqua_client_t* client, const char* device_id);
```

**诊断新增**（snapshot → C API → JNI → Kotlin 同步扩展）：

```text
playback_state          # PlaybackState
route_mode              # FollowSystem / PreferCurrent / PreferredDevice
requested_device_id     # 请求设备（PreferredDevice 时有值）
stream.device_id        # 实际设备（AudioStreamInfo 新增字段；回读值）
switch_result           # Switched / RolledBack / FellBackToSystem / Fatal + AudioError 原因
```

**JNI**：`nativeSetPlaybackDevice(handle, int deviceId /* -1 = 跟随系统 */)`——Android
的 device id 是 int，JNI 层直接编码为 `"android:N"`，Kotlin 无字符串拼接。LongArray
诊断按新字段扩长。

**Kotlin**：

- `AudioDeviceMonitor`（**AquaService 持有**，后台播放期间存活；不放 Activity）：
  设备列表（`AudioManager.getDevices`）、变化通知（`AudioDeviceCallback`）、1s 事件合并

- `AquaController.setPlaybackDevice()`、`playbackState` 轮询、降级横幅
  （"USB DAC 已断开，已切换到系统输出" / "切换失败，继续原设备"）

- UI：设置 → 播放 →"自动切换播放设备"（默认开，持久化）；主页连接卡右侧设备按钮 →
  弹层列表（✓跟随系统 / 各设备，设备类型图标在 Kotlin 层分类，不进 C++）

**不改动**：gRPC/UDP 协议、JitterBuffer、Server 全部、RuntimeState、现有 start/stop 语义、
`AudioPlaybackConfig` 结构。

## 10. 目录与命名

```text
audio/playback/
├── audio_playback.h
├── audio_playback_config.h
└── playback_manager.h     # ClientRuntime 私有内部成员，暂不独立 .cpp
```

- 内部编排类命名 **PlaybackManager**（管理 stream 生命周期 / restart 事务 / 回滚，
  不含策略）。

- **禁止**出现 `routing/ policy/ graph/ endpoint/` 目录或公开 Router 类——这些词会诱导
  架构膨胀。未来出现多输出/每连接多设备/优先级策略时再抽。

## 11. 实施阶段

### Phase A-0（core，先证底线）

只实现 `set_playback_device(nullopt)`：stop 当前流 → 以同参数 start（同 JB、同格式）。
不涉及任何设备 id。验证与测试：

- `restart_playback while callback active`（死锁 / 双重消费）

- JB seq 连续性（restart 前后 playhead 不重置、不重新 pre-roll）

- Runtime 状态正确（Running + Switching → Running）

### Phase A-1（core，完整事务链）

- PlaybackState + 诊断链路（snapshot → C API → JNI → Kotlin）

- PlaybackManager：三元 fallback 链、去重、回滚、重试上限、Fatal

- supervision / CLI timer 改造（§6）

- runtime 级单测（mock playback：回滚正确性、兜底、上限、Fatal → stop）

### Phase B（Android 落地）

AAudio（`resolve` 放行 + `setDeviceId` + 回读）→ C API/JNI → `AudioDeviceMonitor` →
Controller → UI（开关 + 设备弹层 + 横幅）。

### Phase C（Windows，可选）

`OnDefaultDeviceChanged` → 自动 restart（CLI 跟随系统）。手动切换用 `--device-id`
重连已可达成，无紧迫性。

## 12. 实现风险排序

| 部分                               | 风险     | 缓解                  |
| -------------------------------- | ------ | ------------------- |
| thread lifetime（stop/join vs 回调） | **最高** | Phase A-0 专项测试先行    |
| rollback correctness             | 中高     | mock playback 全场景单测 |
| restart transaction              | 中      | A-0 → A-1 渐进        |
| PipeWire（未来）                     | 中      | 契约已冻结，属新后端工作        |
| PlaybackState/diagnostics        | 低      | 照抄现有链路模式            |
| AAudio device id / WASAPI        | 低      | builder 一行 / 零改动    |

Phase A 的重点不是设备，而是**证明"Playback restart 不破坏 JB SPSC 和 Runtime 生命周期"**。
这一点通过以后，Android/WASAPI/PipeWire/CoreAudio 都只是 backend 接入问题。

## 13. 明确不做的事

- 任何格式的 converter / resampler / channel mixer（client 侧也不做）

- gRPC 格式通知、能力协商、server 转码

- native Android 设备注册表（设备世界留 Kotlin）

- 公开的 PlaybackRouter 类

- `AudioDevice.type` 进 C++（设备分类是 UI 标签，留 Kotlin）

- 设备选择持久化

- CoreAudio HAL 热切换特权路径（跨后端行为一致性优先）

- 全设备遍历式 fallback

