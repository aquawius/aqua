# 播放设备切换设计决议

本文冻结客户端播放设备切换（流切换）的架构。讨论时间：2026-09-02；实施前最终决议， 实现不得偏离本文。背景：Android 侧 AAudio
后端未实现设备层，流一旦建立无法切换到其他 输出设备（蓝牙耳机接入、USB DAC 插入等场景）。关联文档：`aaudio_backend_design.md`
（§3 设备路由）、`buffer_design.md`（JitterBuffer 模型）。

## 1. 三个概念的分离

本设计的基础是把三个容易混淆的生命周期彻底拆开：

| 概念     | 含义         | 载体                                     | 切换设备时   |
|----------|--------------|------------------------------------------|--------------|
| Session  | 网络音频会话 | gRPC / UDP / JitterBuffer / session\_id  | **纹丝不动** |
| Playback | 本地音频消费 | 一条 AudioPlayback 流（JB 的唯一消费者） | 销毁重建     |
| Route    | 声音去哪里   | `AudioRoute`（路由权 + 目标端点）+ `RoutePolicy`（意图归属 + 丢失动作），均为 connection 属性 | 按事务链变更 |

Server 与网络协议对客户端的设备切换 **零感知**。

## 2. 冻结原则

1. **Session format is immutable** —— 会话内音频格式不变。无协商、无转码、无 server 通知。
2. **Device switching is client-local** —— 设备切换只影响客户端 playback。
3. **Playback restart ≠ session restart** —— 播放与网络生命周期独立；JB、seq、playhead 跨设备切换保持连续，不清空、不重新
   pre-roll。
4. **Never stay silent voluntarily** —— 切换失败沿固定 fallback 链降级，只有格式不兼容 （链耗尽）才终止音频。
5. **No professional routing** —— 不做 converter、动态格式协商、多输出图、全设备遍历。 Aqua 停在消费级软件这一侧。
6. **不为形式统一制造桥接** —— Android 的设备发现/通知留在 Kotlin 层（`AudioManager`）， 不建 native 设备注册表。跨平台统一抽象的边界就是
   `AudioPlaybackConfig.route`（`AudioRoute`，见 §4）： 谁产生 device id 不重要，各平台可以不同。

## 3. 状态模型

### 3.1 RuntimeState（不动）

`Created / Starting / Running / Degraded / Stopping / Stopped`，语义收窄为 **网络会话生命**。
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

**Fatal 的定义必须明确**：它不是普通的 playback 失败，而是 fallback 链耗尽的终态。 supervision 观察到 `Fatal` 后 `stop()`
整个 runtime（§6），因此 `Running + Fatal` 只在 supervision tick（500ms）内短暂共存。日志中看到 `PlaybackState=Fatal`
即代表会话即将终止。

### 3.3 合法组合

| RuntimeState | PlaybackState | 含义                                |
|--------------|---------------|-------------------------------------|
| Running      | Running       | 正常播放                            |
| Running      | Switching     | 切换中（网络正常，UI 不应变红）     |
| Running      | Starting      | 首流建立中                          |
| Running      | Fatal         | 瞬态（≤500ms，supervision 将 stop） |
| 其他         | Inactive      | 未连接 / 已停止                     |

## 4. 路由模型

路由是 **connection 属性**，不持久化（设备 id 跨会话不稳定）；每次连接按设置起步。 唯一持久化的是"自动切换播放设备"开关。

```cpp
// 轴 1（audio/devices/audio_route.h）：交给 backend 的**请求**——路由权归谁。
enum class AudioRouteAuthority : unsigned char { System, Application };
struct AudioRoute {
    AudioRouteAuthority authority = AudioRouteAuthority::System;
    std::optional<AudioDeviceId> device;      // 仅 Application 权威下是载荷
    std::optional<AudioDeviceId> endpoint_request() const;  // backend 读这个
};

// 轴 2（audio/devices/route_policy.h）：manager 的**决策状态**。
enum class RouteIntentOwner : std::uint8_t { None = 0, Application = 1, User = 2 };
enum class RouteLossAction  : std::uint8_t { FallbackToSystem, Fatal };
struct RoutePolicy {
    RouteIntentOwner owner;
    RouteLossAction  on_loss;
    std::optional<AudioDeviceId> intent;      // sticky 目标；fallback 不覆盖它
};
```

两根轴正交：`AudioRoute` 回答"这条流绑到哪个端点、路由权归谁"，`RoutePolicy` 回答 "意图归谁、钉住的设备丢了怎么办"。

- `System` = 平台持有路由权并保留后续改路由的权力（AAudio 不调 `setDeviceId`、 WASAPI 用默认 endpoint、PipeWire 交 session manager、Core Audio 用默认
  device）； `Application` = 这条流绑定到单一端点。 **跨平台推论**：流一旦绑定，系统默认 输出的变化就不再移动它——这在所有平台都成立，不是
  Android 特例。
- client 侧 `on_loss` 恒为 `FallbackToSystem`（`PlaybackManager::kLossAction`，移动端 永不主动静音），与 server 采集侧恒为 `Fatal`
  （`CaptureManager::kLossAction`， 绝不静默换采集源）相反。这个 server/client 不对称由 `RouteLossAction` **显式**表达在一处，
  不再靠两个同名不同义的枚举值隐含（见 §16.1）。

UI 映射：

| 用户动作                          | `RouteIntentOwner`                    | 诊断标签           |
|-----------------------------------|---------------------------------------|--------------------|
| 设置"自动切换播放设备" ON（默认） | `None`                                | `follow_system`    |
| 设置"自动切换播放设备" OFF        | 首流成功后升级为 `Application`        | `prefer_current`   |
| 弹层选"跟随系统"                  | `None`                                | `follow_system`    |
| 弹层选具体设备                    | `User(id)`                            | `preferred_device` |

> `User` 的语义是 **优先**而非 **固定**：设备不可用时按 §5 的链降级，不无限等待。
> 这是与"FIXED 永不偷偷切"的旧表述的有意决裂——永不主动静音优先。
> `User` 是 **sticky** 的：设备回归时自动切回（§14.2）。`Application` 是 App 自动钉住 首流的实际落点，**非
> sticky**：它表达的只是"别乱跑"，不是"我认准了这个"，因此设备回归 **不**自动切回。

"自动切换播放设备" OFF 的连接起步是两段式的：请求仍是 `AudioRoute::follow_system()`， `RoutePolicy::from_route`
先记 `None`；`start()` 成功后回读 `stream_info().device_id`， 读到实际落点才升级为 `RoutePolicy::app_pinned` 并把
`active_config_.route` 改成 `AudioRoute::pin(actual)`。后端读不到落点（AAudio 回读 `UNSPECIFIED`）时保持
`None`，退化为跟随系统的 restart 语义。

错误驱动 restart 的目标由 `RoutePolicy::restart_target(actual)` 推导：`None → 系统默认 (nullopt)`；`Application → intent`（读不到则退回
`actual`，即首流实际落点）； `User → intent`（sticky，fallback 降级后仍指向用户钉住的设备，而非当前兜底设备）。

**启动期设备兜底**：带 `--playback-device-id` 的首次 `PlaybackManager::start()` 失败时，
以系统默认设备重试一次并记日志，避免单个设备不可用直接导致连接失败；重试失败才整体失败。 该兜底不改变路由策略（仍按上文由
`config.route` 推导）。

## 5. 统一 restart 事务链

所有切换场景（手动选择 / 设备拔出 / 流断开错误）收敛到 **同一个算法**，由 PlaybackManager（ClientRuntime 私有内部成员，见
§10）执行——错误驱动的恢复经
`on_playback_event` 即时 `asio::post` 派发到 ioc 线程就地执行（检测延迟 ~20ms， 不等待 supervision 的 500ms tick），手动切换经
C API 控制路径同步调用：

```text
set_playback_device(target):            # target 由 RoutePolicy 推导或用户直接给出
    PlaybackState = Switching
    active_generation = kNoStreamGeneration   # 先"退认"当前流（§16.2）：teardown
                                              # 期间到达的事件在 manager 侧即被丢弃
    捕获 previous_active_device         # 来自 AudioStreamInfo 的实际设备回读
    stop 旧流（同步 join 回调线程）      # break-before-make，保证 JB 消费者唯一

    candidates = 去重([                  # 形态由 target 决定
        target_device,                  # 显式目标才有 fallback 链；
        previous_active_device,         # nullopt（自动/用户跟随）单候选直达
        system_default,                 # 当前默认，不回滚 previous
    ])

    for c in candidates:
        if start(c, 会话契约格式) 成功:
            更新路由策略与实际设备；active_generation = backend.generation()
            PlaybackState = Running
            上报 switch_result（含降级原因，驱动 UI 横幅）; return

    # 仅显式 target：链全灭后先重试刚关闭的上一设备（瞬时失败才重试，
    # 见下"break-before-make 的代价"）；成功即 RolledBack，会话继续。
    if target 有值 and 上一设备有值 and 最后的错误是瞬时类:
        for attempt in 1..4:                     # 200ms 线性退避，合计约 2.1s（200+400+600+800）
            sleep(200ms * attempt)
            if start(上一设备) 成功:
                上报 switch_result = RolledBack; return
            最后的错误 = 本次错误
            if 最后的错误不是瞬时类: break

    PlaybackState = Fatal               # 链耗尽（含重试）= 终态
```

**break-before-make 的代价（2026-09 补）**：`switch_to` 先 `stop()` 旧流再
`start()` 新流，而移动端的 A2DP / USB 音频摘除是 **异步**的——刚关闭的上一设备 常常在几百毫秒内重开失败；同时系统默认此刻往往仍是同一台设备，于是
`nullopt` 与
`previous` 解析到同一落点，三层链实际退化成一层，一次瞬时失败就直达 `Fatal`， 把 **整条连接**
停掉（用户观感：换个设备结果断线）。因此链耗尽后对"上一设备"做 **有界重试**（最多 4 次、线性退避、合计约 2.1s，仅对 `DeviceUnavailable / DeviceDisconnected /
BackendFailed` 这类瞬时错误），能回去就 `RolledBack` 保住会话；回不去才 `Fatal`。
`nullopt`（自动跟随 / 用户跟随）事务 **不重试**：跟随语义下旧设备正是要离开的那个， 回滚它只会延迟失败并引发横跳。

要点：

- **意图归属按用户请求推导**：`set_playback_device(id)` 成功即 `RouteIntentOwner::User`
  （即使本次 fallback 降级到系统默认，`RoutePolicy::intent` 与自动切回保留）；`set_playback_device(nullopt)`
  即 `None`。fallback 降级不改变用户意图。
- **链固定三层**（显式目标），不做全设备遍历（原则 5 的直接推论）。链条冗余重试无害（start 幂等）。

- `previous_active_device` 与 `system_default` 可能解析到同一物理设备，仅按 id 相等去重。

- **JB 不清空**：切换间隙 JB 水位上涨，deadline-high Drop 自动跳到最新；新流接上后 play\_seq 连续。切换逻辑完全不碰
  JitterBuffer。

场景走查：

| 场景                              | candidates 实际展开          | 结果                             |
|-----------------------------------|------------------------------|----------------------------------|
| 手动选 USB DAC，格式 OK           | \[DAC]                       | 无感切换                         |
| 手动选 USB DAC，FormatUnsupported | \[DAC → 旧设备 → SYSTEM]     | 回滚旧设备 + 横幅"切换失败"      |
| User 意图的 DAC 被拔              | \[DAC(跳过) → SYSTEM]        | 立即落 SYSTEM + 横幅"设备已断开" |
| 跟随系统下系统 reroute 杀流       | \[SYSTEM]                    | 重开即跟随新默认                 |
| Application（prefer_current）下流死亡 | \[旧设备 → SYSTEM]       | 旧设备还在则原地重开             |
| SCO/HFP 接入（16k mono 不兼容）   | 链耗尽（非瞬时错误，不重试） | Fatal → stop                     |
| 手动切到扬声器，蓝牙异步摘除中    | 三层链全灭 → 重试上一设备    | RolledBack（会话保住）           |
| 切换后新流 6~8ms 即 DISCONNECTED（路由未稳定，§16.3） | \[同一目标重开]，走 settle 预算 | 节流即返回不改状态，由 supervision tick 再驱动；**不占**设备丢失预算 |

**防抖与重试上限**：错误驱动的自动 restart 与内部自动跟随（tick 轮询、快照新增、 自动切回）在 10s 窗口内共享最多 3 次，超过按链耗尽处理
（防蓝牙连接风暴造成重启死循环）。用户显式选择不计数并重置窗口。Kotlin 侧设备事件 做 1s 合并窗口，最新目标胜出。

**这套预算只管"设备丢失"**（rev3 修订）：流刚起来就死属于"路由未稳定"，走一套 **完全独立**的预算与节流，绝不占用这里的额度——两者混在一个预算里，会让一次
250ms 的路由抖动烧光名义上"10 秒 3 次"的额度并直接 Fatal（实测：4 次尝试全挤在 256ms 内，会话被断）。判据、常量与节流语义见 §16.3。 用户显式
`set_playback_device()` 同时重置 **两套**窗口。

## 6. supervision 边界

`aqua_capi.cpp` 的 supervision tick 与 CLI control timer 同步改为：

```text
heartbeat_failed                      → 不动作     # 只看 Degraded（锁存与 Degraded 同 tick）
RuntimeState::Degraded（网络原因） → stop()     # 保留
PlaybackState::Fatal              → stop()     # 唯一新终止条件
PlaybackState::Switching / 设备错误 → 不动作    # 不再误杀会话
```

恢复触发的两条路径（`ClientRuntime::service_playback_recovery`）：

1. **错误标志驱动**：backend 运行期错误经 event callback 即时投递（AAudio
   `report_fatal_once` / WASAPI 事件线程），事件带 stream generation， **先在
   PlaybackManager 内按归属过滤**（§16.2）：不属于当前流的事件计入
   `stale_events_dropped` 后丢弃，根本不上到 runtime。放行者由 `on_playback_event` 置标志并
   `asio::post` 到 ioc 就地执行 restart 事务。例外（rev3 起降为 **第二道防线**，刻意保留）：
   **Switching 期间投递的 事件不派发**——generation 过滤拦的是"不属于当前流"的事件，而"新流已 认领、state 尚未翻回
   Running"的窄窗口仍只有这道 gate 覆盖。路由由事务自身 的候选链负责，避免一次手动切换叠加多余的 restart（双重 stop/start 会拉长
   静音窗口并消耗重试预算）。
2. **静默死流兜底**：supervision tick 发现「PlaybackState=Running 但 backend is_running ()=false」（回调线程已死、错误未被投递的场景——如
   AAudio data callback 返回 STOP 后流静默停止），按设备错误同等处理走同一 restart 事务。此前该场景无任何检测：JB
   只进不出被打满、输出永久静音。

## 7. 线程模型与 SPSC 保护

| 约束                 | 方案                                                                                                                                                               |
|----------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| JB SPSC 契约         | break-before-make：`stop()` 同步 join 旧回调线程后才 `start()` 新流，任何时刻 JB 只有一个消费者                                                                    |
| 事务线程（core）     | 错误恢复经 `on_playback_event` 即时 `asio::post` 到 ioc 就地执行（stop/join/start 阻塞 ~数十 ms，Heartbeat 定时器延迟一拍无害）；手动切换经 C API 控制路径同步调用 |
| 控制串行化（core）   | restart 与 start/stop 同在 runtime 控制路径（C API 经 ioc 调度，与 supervision 同 strand）                                                                         |
| 控制串行化（Kotlin） | 所有 native 生命周期调用继续走 `lifecycleExecutor`；AudioDeviceCallback 在 binder 线程 → mainHandler → controller 决策 → executor 执行                             |
| 死锁防护             | stop 路径不得持有回调路径需要的锁；stop/join 期间回调只做 JB pull 与原子读                                                                                         |

**Phase A-0 必须包含专项测试**：`restart_playback while callback active`——回调正在
`JB.pop()` 时发起 stop/join，验证无死锁、无双重消费、seq 连续。

## 8. 后端契约

统一契约：`AudioPlayback::start(config)`，其中设备选择 **只经** `config.route`（`AudioRoute`） ——backend 读
`route.endpoint_request()`，得到 `nullopt`（跟随系统，**刻意**不带 id：这正是让平台 保有路由权的原因）或具体
id；`config.format` = 会话契约格式， **永不由后端改写**。

backend 另需实现 `generation()`（ **纯虚**）：每次成功 `start()` 递增的单调流代号，无流 在跑时为
`kNoStreamGeneration`；事件回调只允许上报自己那条流的 generation（§16.2）。 定为纯虚是刻意的——漏实现的后端会让它上报的所有事件被静默丢弃，这种失效必须是编译错误， 不能是运行期惊喜。

**Invariant**：`start()` 成功 ⇒ 后端以请求的 encoding + channels 消费数据； **采样率允许 平台透明 SRC**（AAudio 既有决议，
`aaudio_backend_design.md` §1.2——系统内重采样不改变 JB 的时钟基准，不违反 session immutability）。后端不得偷偷改变
channels/encoding 后返回 成功；不支持即返回 `FormatUnsupported`。

| 后端      | device 传入                      | 实际设备验证                                                             | 格式不兼容来源                           | 增量工作                                            |
|-----------|----------------------------------|--------------------------------------------------------------------------|------------------------------------------|-----------------------------------------------------|
| AAudio    | `"android:N"` → `setDeviceId(N)` | open 后 `getDeviceId()` 回读进 `AudioStreamInfo`（新增 device\_id 字段） | SCO/HFP（16k mono）                      | `resolve()` 放行 `android:N` + builder 一行 + 回读  |
| WASAPI    | endpoint id（枚举已有）          | 激活的 endpoint 即所请求                                                 | `IsFormatSupported` 预检失败（路径已有） | **零改动**（endpoint 重建天然就是 restart 语义）    |
| PipeWire  | `PW_KEY_TARGET_OBJECT`           | stream 状态机                                                            | 几乎不会（adapter 自动协商）             | 属于写新后端，契约已就位                            |
| CoreAudio | HALOutput `CurrentDevice`        | 属性回读                                                                 | 几乎不会（AudioConverter）               | 同上；坚持 break-before-make，不用 HAL 热切特权路径 |

## 9. 接口面

**C API**（thin，不含策略； **异步请求语义**——调用返回 ≠ 切换完成，结果经诊断观察）：

```c
// device_id == NULL 表示跟随系统（AudioRoute::follow_system()，意图归属 None）
int aqua_client_set_playback_device(aqua_client_t* client, const char* device_id);
```

**诊断新增**（snapshot → C API → JNI → Kotlin 同步扩展）：

```text
playback_state          # PlaybackState
route_mode              # 类型是 RouteIntentOwner（None / Application / User）；
                        # 文本标签仍为 follow_system / prefer_current /
                        # preferred_device（route_intent_owner_label()，逐字节不变）
requested_device_id     # 请求设备（User 归属时有值）
stream.device_id        # 实际设备（AudioStreamInfo 新增字段；回读值）
switch_result           # Switched / RolledBack / FellBackToSystem / Fatal + AudioError 原因
```

C 枚举 `aqua_route_mode`（`AQUA_ROUTE_FOLLOW_SYSTEM=0` / `AQUA_ROUTE_PREFER_CURRENT=1` /
`AQUA_ROUTE_PREFERRED_DEVICE=2`）与 Kotlin 的 `AquaRouteMode` **不变**：`RouteIntentOwner` 的底层取值 就是这三个编码，由
`aqua_capi.cpp` 的 `AQUA_CAPI_ASSERT_ENUM_MIRROR` static assert 锁定。 因此 rev3 对诊断链路是零 ABI 变更（§16.1）。

**JNI**：`nativeSetPlaybackDevice(handle, int deviceId /* -1 = 跟随系统 */)`——Android 的 device id 是 int，JNI 层直接编码为
`"android:N"`，Kotlin 无字符串拼接。LongArray 诊断按新字段扩长。

**Kotlin**：

- `AudioDeviceMonitor`（ **AquaService 持有**，后台播放期间存活；不放 Activity）： 设备列表（`AudioManager.getDevices`）、变化通知（
  `AudioDeviceCallback`）、1s 事件合并

- `AquaController.setPlaybackDevice()`、`playbackState` 轮询、降级横幅 （"USB DAC 已断开，已切换到系统输出" /
  "切换失败，继续原设备"）

- UI：设置 → 播放 →"自动切换播放设备"（默认开，持久化）；主页连接卡右侧设备按钮 → 弹层列表（✓跟随系统 / 各设备，设备类型图标在
  Kotlin 层分类，不进 C++）

**不改动**：gRPC/UDP 协议、JitterBuffer、Server 全部、RuntimeState、现有 start/stop 语义。 （`AudioPlaybackConfig`
原本也在这份清单里，rev3 已把它移出：`std::optional<AudioDeviceId> device` 成员被 **删除**，`AudioRoute route` 成为首位也是唯一的设备选择字段——见
§16.1。）

## 10. 目录与命名

```text
audio/playback/
├── audio_playback.h
├── audio_playback_config.h
└── playback_manager.h     # ClientRuntime 私有内部成员，暂不独立 .cpp
```

- 内部编排类命名 **PlaybackManager**（管理 stream 生命周期 / restart 事务 / 回滚， 不含策略）。

- **禁止**出现 `routing/ policy/ graph/ endpoint/` 目录或公开 Router 类——这些词会诱导 架构膨胀。未来出现多输出/每连接多设备/优先级策略时再抽。

- rev3 的路由词汇（`audio_route.h` / `route_policy.h`）落在 **既有的** `audio/devices/`
  下（与 `audio_device.h` / `audio_device_manager.h` 同目录），**不违反**上一条：没有新建
  `routing/` 或 `policy/` 目录，也没有公开 Router 类——`AudioRoute` 与 `RoutePolicy` 都是值类型， capture 侧共用同一份。
  被删掉的 `playback_route_mode.h` / `capture_route_mode.h` 原本分居
  `audio/playback/` 与 `audio/capture/`，两套并行词汇正是同名不同义的来源（§16.1）。

## 11. 实施阶段

### Phase A-0（core，先证底线）

只实现 `set_playback_device(nullopt)`：stop 当前流 → 以同参数 start（同 JB、同格式）。 不涉及任何设备 id。验证与测试：

- `restart_playback while callback active`（死锁 / 双重消费）

- JB seq 连续性（restart 前后 playhead 不重置、不重新 pre-roll）

- Runtime 状态正确（Running + Switching → Running）

### Phase A-1（core，完整事务链）

- PlaybackState + 诊断链路（snapshot → C API → JNI → Kotlin）

- PlaybackManager：三元 fallback 链、去重、回滚、重试上限、Fatal

- supervision / CLI timer 改造（§6）

- runtime 级单测（mock playback：回滚正确性、兜底、上限、Fatal → stop）

### Phase B（Android 落地）

AAudio（`resolve` 放行 + `setDeviceId` + 回读）→ C API/JNI → `AudioDeviceMonitor` → Controller → UI（开关 + 设备弹层 + 横幅）。

### Phase C（Windows，可选）

自动跟随用 `PlaybackManager::tick()` 在 control tick（500ms）内比较
`AudioDeviceManager::default_device(OUTPUT)` 与当前实际设备实现（ **轮询**，非
`IMMNotificationClient::OnDefaultDeviceChanged`——代码库无 COM 通知既有基建，轮询与 本文 §5 的 poll 哲学一致，且与 Android
推送路径互不干扰）。该 tick 由
`ClientRuntime::service_default_device_follow()` 在 lifecycle_mutex_ 下转发，CLI control timer 驱动。手动切换用
`--playback-device-id` 重连已可达成，无紧迫性。

## 12. 实现风险排序

| 部分                                 | 风险     | 缓解                     |
|--------------------------------------|----------|--------------------------|
| thread lifetime（stop/join vs 回调） | **最高** | Phase A-0 专项测试先行   |
| rollback correctness                 | 中高     | mock playback 全场景单测 |
| restart transaction                  | 中       | A-0 → A-1 渐进           |
| PipeWire（未来）                     | 中       | 契约已冻结，属新后端工作 |
| PlaybackState/diagnostics            | 低       | 照抄现有链路模式         |
| AAudio device id / WASAPI            | 低       | builder 一行 / 零改动    |

Phase A 的重点不是设备，而是 **证明"Playback restart 不破坏 JB SPSC 和 Runtime 生命周期"**。
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

## 14. 修订 rev2（2026-09-03）：设备事件推送模型与错误通道分离

rev1 落地后发现两处架构缺口，本次修订冻结以下变更（capture 侧不在本修订范围）：

### 14.1 设备事件推送模型：路由策略全部收回 core（修订 §5/§9）

rev1 的跟随触发是拉模型（`PlaybackManager::tick()` 轮询系统默认设备），但 Android 的 `default_device` 返回合成空 id，tick 在
Android 上是 no-op——跟随/回退/合并窗口 被迫在 Kotlin 层（`AquaController`）重新实现，同一份路由语义出现两份实现。

rev2 改为推送模型：

```c
// C API：当前可选输出设备 id 全集快照（后端词汇，Android 经 JNI 编码 "android:N"）
void aqua_client_notify_devices_changed(aqua_client_t* client,
    const char* const* present_ids, int32_t count);
```

- 设备发现留在平台层（Android = Kotlin `AudioManager`），core **不建**设备注册表， 只消费事件快照（§2.6 原则不变——快照是事件载荷，不是注册表）。
- 1s 合并去抖从 Kotlin 上收到 core（`ClientRuntime`，ioc 线程，最新快照胜出）。
- 决策全部在 `PlaybackManager::on_devices_changed`：
    - 活跃设备不在集合 → eager restart（`restart_on_error` 路径：路由推导目标 + fallback 链 + 重试预算； **保留
      `RoutePolicy`**）；
    - `RouteIntentOwner::None`（跟随系统）且有新增设备 → 内部跟随系统默认（默认可查询且确实变化时 才重开，否则跳过；与错误驱动共享重试预算，不重置窗口）；
    - `User` 且意图设备回归 → **自动切回**（同样消费重试预算； 失败回滚后用户意图仍保留，下次回归可重试）；
    - `Application` → 仅活跃设备消失时动作。
- 每份连接的首份快照只作基线，不触发决策（初始列表不是"新增设备"）。
- Kotlin 侧删除全部路由策略（`followSystemDefaultIfEligible` /
  `fallbackIfCurrentDeviceGone` / 合并窗口），`AudioDeviceMonitor` → Controller 退化为纯事件转发器。Windows GUI 端将来零策略代码。
- `tick()` 轮询保留（WASAPI 平台推送未接入前的既有跟随机制，与推送模型并存）。

### 14.2 sticky 用户意图（rev3 起并入 `RoutePolicy::intent`）：修复 fallback 丢失用户选择

rev1 把用户意图存在当时的 `active_config_.device`，fallback 降级时它被覆写为兜底设备—— 用户钉住的设备被拔一次，选择就永久丢失（Kotlin
的 eager 回退更是直接把意图归属改成跟随系统）。rev2 增加独立成员 `preferred_device_` （rev3 起该成员即
`RoutePolicy::intent`，语义不变，见 §16.1）：

- `set_playback_device(id)` 记入，`set_playback_device(nullopt)` 清除，`start()` 按
  `config.route` 初始化； **fallback 链不触碰它**——这是"优先而非固定"语义的载体。
- `restart_on_error` 在 `User` 归属下的目标 = `intent`（而非 当前的 `active_config_.route`
  ），设备回归后自动切回以其为目标。
- 诊断 `requested_device_id` 报告 sticky 意图（降级期间仍显示用户的选择）。

**产品决议（2026-09-03）**：钉住设备重新接入时 **自动切回**（参照微信电话的 设备选择体验）；切回失败回滚后意图保留，下次回归可重试。

### 14.3 错误通道与诊断快照分离（修订 §9 诊断新增）

rev1 的 `last_audio_error` 是锁存残值（置位后永不清零），且混在诊断快照里—— 快照被迫承担错误传递。rev2 分离：

- 诊断快照（`ClientDiagnosticsSnapshot` / `aqua_client_diagnostics_t`） **移除**
  `last_audio_error` 字段——快照回归纯组件状态。
- 错误走独立通道：`last_audio_error()` + `audio_error_epoch()`（C API：
  `aqua_client_get_last_audio_error` / `aqua_client_get_audio_error_epoch`）。实现上两者是 **同一个 64 位原子**
  （`audio_error_state_`：低 8 位 = 错误值，高位 = epoch），置位与清零各走一次 CAS——分成两个原子会让轮询方 读到"新错误 + 旧
  epoch"而漏掉一次事件。`AudioError` 必须塞进低 8 位，由 `static_assert` 锁定。
- 语义： **值变化递增 epoch**（置位新错误 / 恢复清零）；成功的恢复事务清零错误。 清零须覆盖 **两条触发路径**：
    - **错误驱动**：`service_playback_recovery` 事务成功后 `clear_audio_error()`。
    - **notify 驱动**：`service_devices_changed` → `on_devices_changed` 的 eager restart / 自动切回事务成功后也须清零（rev2
      初版漏掉此路径，导致"设备已断开"
      作为残值永久锁存在错误通道、Android 状态横幅持续显示）。触发条件：事务被触发 （`acted=true`）且完成后仍处于 `Running`。
      轮询方以 epoch 变化检测"新错误"与"已恢复"，不再出现"设备已断开"残留。
- 时序安全：旧流临终错误在事务 `stop()` 阶段 latch（`join` 保证先于事务返回）， 清零必在其后；链耗尽 → Fatal（非 `Running`
  ）保留错误供停止原因查询，两条路径 语义一致。
- Fatal / 停止路径不清零——停止原因查询（`stopReasonOf`）不受影响。
- Server 侧 `last_audio_error` 不在本修订范围（随 capture 切换设计一并处理）。
- 配套 UX 决议（Android）：播放中的音频错误由 core 自动恢复，只弹 **瞬时横幅**
  （与"已切换播放设备"同款），不再锁存进状态横幅；致命错误仍随 STOPPED 由
  `stopReasonOf` 显示。设备监视器上移至 MainActivity 进程级持有（App 启动即 推送快照，设备列表不依赖连接）。

## 15. 切换事务序号（`switch_seq`）

每笔切换事务（成功 / 回滚 / 兜底 / 链耗尽）递增一次，暴露在诊断快照
`ClientDiagnosticsSnapshot::switch_seq`（结构体 **末尾**字段，C API 只能末尾追加）与 Android 诊断数组里；CLI 侧见诊断行
`switch_seq`。

**为什么需要**：`outcome + error` 只能表达"最近一次结果"——两笔不同的事务可能完全相同 （都是 `Switched` + `None`），UI
无法判定"又切了一次"；设备 id 在部分平台回读为空， 也不能当唯一判据。因此序号是 UI 判定"又发生一次切换"的唯一可靠信号 （来源：
`PlaybackManager::switch_seq()`）。

**排障用法**：序号持续增长但音频无异常 = 静默自愈在正常工作；序号不增长而用户报卡顿 = 问题不在设备切换（去看 JB 与网络，见
`operations_and_troubleshooting.md` §8）。

## 16. 修订 rev3（2026-09-25）：路由两根轴、事件归属与 settle 预算

rev2 落地后在 Android 真机上暴露出一个会把 **整条连接**弄断的缺陷（§16.3），修它需要先把"路由"这个词拆开（§16.1）， 并把"这个错误属于哪条流"变成可判定的事实（§16.2）。本次修订冻结以下三项。

### 16.1 路由模型拆成两根正交的轴（修订 §4 / §8 / §9）

**删除** `audio/playback/playback_route_mode.h`（`enum class PlaybackRouteMode { FollowSystem, PreferCurrent, PreferredDevice }`
与 `playback_route_mode_name()`）与 `audio/capture/capture_route_mode.h`（`enum class CaptureRouteMode { FollowSystem, PreferredDevice }`
与 `capture_route_mode_name()`）。

原因：两组枚举各自把 **两个独立问题**——"路由意图归谁"与"钉住的设备丢了怎么办"——压扁成一个扁平取值。后果是
`PlaybackRouteMode::PreferredDevice`（优先 + 降级，永不静音）与 `CaptureRouteMode::PreferredDevice`（严格钉住， Fatal，绝不换源）
**同名而语义相反**。server/client 的这份不对称过去只隐含在两个同名枚举值里，读代码的人无处可查。

取代它的是两根正交的轴：`audio/devices/audio_route.h`（给 backend 的 **请求**）与 `audio/devices/route_policy.h`（ manager
的 **决策状态**），形态见 §4。既有语义的精确映射：

| 删除的取值                        | `RouteIntentOwner` | sticky | 设备回归自动切回 | 诊断标签（不变）   |
|-----------------------------------|--------------------|--------|------------------|--------------------|
| `PlaybackRouteMode::FollowSystem` | `None`             | —      | —                | `follow_system`    |
| `PlaybackRouteMode::PreferCurrent`| `Application`      | 否     | **否**           | `prefer_current`   |
| `PlaybackRouteMode::PreferredDevice` | `User`          | 是     | 是               | `preferred_device` |
| `CaptureRouteMode::FollowSystem`  | `None`             | —      | —                | `follow_system`    |
| `CaptureRouteMode::PreferredDevice` | `User`           | 是     | 无此路径（见下） | `preferred_device` |

`Application` = App 自动钉住首流的实际落点，表达"别乱跑"而非"我认准了这个"，因此 **不** sticky、设备回归 **不**自动切回。

「设备回归自动切回」一栏对 capture 标"无此路径"而不是"是/否"，是刻意区分 **策略**与**能力**：
`RoutePolicy::auto_returns()` 只看归属（`owner == User` 即 true），所以采集侧的 policy 同样"愿意"切回；但
`CaptureManager` **没有** `on_devices_changed` 这条设备集合推送路径（server 侧只有 `tick()` 轮询系统默认设备，
且钉住意图下 `tick()` 直接不动作），因此没有任何调用点会去消费这个意愿。等 server 将来接上 GUI 与设备推送，
这条路径补上即可，policy 侧无需改动。

丢失动作则成为显式常量，两端相反且必须相反：

```cpp
PlaybackManager::kLossAction = RouteLossAction::FallbackToSystem;  // client：移动端永不主动静音
CaptureManager::kLossAction  = RouteLossAction::Fatal;             // server：绝不静默替换采集源
```

capture 侧 **永不**使用 `RouteIntentOwner::Application`——server 没有交互界面，不存在"保持我们落到的那个"的用户语义。

配套变更：

- `AudioPlaybackConfig` / `AudioCaptureConfig` **删除** `std::optional<AudioDeviceId> device` 成员，`AudioRoute route`
  成为首位也是唯一的设备选择字段。任何写 `config.device` 的地方都已失效。
- 两个 manager 的 `route_mode()` **移除**，代之以 `intent_owner()`（原子，任意线程可读，诊断投影）与 `route_policy()`（完整
  `RoutePolicy`，**仅控制线程**——它含 `std::string`）。 `PlaybackManager` 原 `preferred_device_` 成员即
  `RoutePolicy::intent`。两个 manager 各新增 `stale_events_dropped()`。
- **对外零变更**：`ClientDiagnosticsSnapshot::route_mode` 与 `ServerDiagnosticsSnapshot::CaptureSwitchStats::route`
  只是把类型换成 `audio::RouteIntentOwner`；C 枚举 `aqua_route_mode` 与 Kotlin `AquaRouteMode` 取值不动
  （`RouteIntentOwner` 的底层值按构造就是那三个编码，由 `aqua_capi.cpp` 的
  `AQUA_CAPI_ASSERT_ENUM_MIRROR` static assert 锁定）；`snapshot_view.cpp` 用
  `route_intent_owner_label()` 渲染，输出字符串与改动前逐字节相同。UI 与日志看不出区别。

### 16.2 流事件归属契约（stream generation）

新增 `audio/audio_stream_event.h`：

```cpp
using StreamGeneration = std::uint64_t;
inline constexpr StreamGeneration kNoStreamGeneration = 0;   // 0 保留给"没有流"

struct AudioStreamEvent {
    AudioError error;
    StreamGeneration generation;
    bool is_device_loss() const;   // DeviceDisconnected | DeviceUnavailable | DeviceNotFound
    bool empty() const;            // error == None
};
using AudioStreamEventCallback = compat::MoveOnlyFunction<void(const AudioStreamEvent&) noexcept>;
```

`AudioPlaybackEventCallback` 与 `AudioCaptureEventCallback` 现在都是 `AudioStreamEventCallback` 的别名（此前是
`void(AudioError) noexcept`）。

**为什么必须带来源**：manager 复用 **同一个** backend 实例跨越多次 stop/start（切换事务就是 stop → start，不重建
backend）。因此一个不带来源的 `AudioError` 在上层 **不可判别**——"我刚起的流死了"与"上一条流的迟到讣告"要求的响应 完全相反（前者要恢复，后者要忽略）。

契约要点：

- `AudioPlayback::generation()` 与 `AudioCapture::generation()` 是新增的 **纯虚**方法：每次成功 `start()` 单调递增， 无流在跑时为
  `kNoStreamGeneration`。定为纯虚是刻意的——漏实现的后端会让它上报的所有事件被静默丢弃， 这种失效模式必须是编译错误。
- generation **必须来自一个独立的单调计数器**，不能复用 `stop()` 会清零的那个原子。共用一个原子会让 stop → start
  重新发出同一个号码，于是旧事件可以冒充新流。三个 backend 都把 `generation_counter_`（永不重置） 与"当前生效值"分开保存。
- manager 侧持有 `active_generation_`：每次 `start_stream` 成功后从 `backend->generation()` **认领**；在每笔事务
  （`switch_to` / `restart`）开头以及 `stop()` 里，**先退认**（置 `kNoStreamGeneration`）再调
  `backend->stop()`。包装后的事件回调丢弃 generation 不等于 `active_generation_` 的事件，并计入
  `stale_events_dropped_`。
- 这把"旧流临终错误"的 **主过滤器**从 `ClientRuntime` 的 `PlaybackState::Switching` 时间窗 gate 下移到 manager——由 猜测变成精确判定（错误可能在新流已启动之后才到，时间窗判不了）。runtime 侧的 gate
  （`on_playback_event` / `on_capture_event`） **刻意保留**为第二道防线，覆盖"新流已认领、state 尚未翻回 Running" 的窄窗口（§6 路径 1）。
- **可观测量**：两个 manager 的 `stale_events_dropped()`。非零是确认 provenance 过滤真的拦下了东西的唯一途径。

AAudio 侧（`src/audio/playback/aaudio/`）：

- 传给 `AAudioStreamBuilder_setDataCallback` / `setErrorCallback` 的 `user_data` 不再是 `this`，而是 per-stream 的
  `StreamSlot { AAudioAudioPlayback* self; StreamGeneration generation; std::shared_ptr<CallbackContext> context; }`。
- backend 持有 `current_slot_` 与 **恰好一个** `retired_slot_`：AAudio 的 error callback 与
  `AAudioStream_close()` **不同步**（close 只保证 data callback 已返回），因此每条已退役流的一次迟到投递必须仍然安全。
- `report_fatal_once(const StreamSlot&, AudioError)` 先判 `slot.generation == live_generation_`，才允许触碰
  `running_` / `pending_error_` / `event_callback_`；`stop()` 的 **第一个动作**就是清掉 `live_generation_`。
- **stream 指针本身不当身份令牌**：`close()` 之后新流可能分配到同一地址（ABA），指针身份不成立。
- data callback 从 **自己的槽位**读上下文，不再读共享的 `callback_context_` 成员——在途回调因此不可能拿到新流的几何。
- error callback 的日志行同时打印 `stream_generation` 与 `live_generation`（排障用法见
  `operations_and_troubleshooting.md` §6）。

WASAPI 侧：事件线程签名改为 `event_thread_main(StreamGeneration)`，generation 在 spawn 时 **按值**捕获。

### 16.3 路由未稳定（settle）预算：修复 Android 切换断连（修订 §5）

**现象**：Android 上从钉住的蓝牙耳机切到钉住的内建扬声器，会话被整个停掉，用户观感是"换个设备把连接搞断了"。

**证据**（`temp/android_switch.log`，蓝牙 `android:2733` → 内建扬声器 `android:3`）：

- 每条新起的流都在 `AAudioStream_requestStart` **成功之后 6~8ms** 收到 `AAUDIO_ERROR_DISCONNECTED`
  （14:09:43.359→.365、.418→.425、.478→.485、.541→.549）。
- 四次尝试全挤在 **256ms** 内，烧光名义上"10s 内 3 次"的设备丢失预算 → `PlaybackState::Fatal` → supervision
  停掉整个 `ClientRuntime`。
- 这些 **不是**旧流的迟到讣告。反证：14:09:18.430 一条已跑 4.6s（起于 14:09:13.815）、期间无任何 stop/close 的流 自发收到
  DISCONNECTED，而它处于 `follow_system`——`setDeviceId` 从未被调用过。14:09:40.236 同理（流龄 614ms， 无
  close）。结论：Android 的 AudioPolicy 在设备转换期间会重新路由，并把 DISCONNECTED 投递给 **当前、活着**的流。
- 加重因素即 `aaudio_backend_design.md` §3.1 记的那条：`setDeviceId` 只对 USB/BT 外接设备可靠，对内建设备行为未定义。
  但它 **不是唯一原因**——同一份日志里一条钉住 **蓝牙**（外接、id 可靠）的流也在 start 后 17ms 收到 DISCONNECTED
  （14:09:39.559→.576）。

**修法**：在 `PlaybackManager::restart_on_error()` 里把失败分类，两类各走各的预算。

```text
settling = active_generation_ 有效            # 当前确实认领着一条流
        && !is_running()                      # 那条流**真的死了**（backend 已置 running_=false）
        && 流龄 < kRouteSettleWindow           # 死得太快，快得不像设备被拔掉
```

三个判据 **缺一不可**：只看时间会把"流还活着、上层因别的原因要求 restart"误判成路由抖动。

| 预算           | 常量                                            | 适用                       |
|----------------|-------------------------------------------------|----------------------------|
| 路由未稳定     | `kMaxSettleRestarts` = 8 / `kSettleWindow` = 5s，最小间隔 `kSettleRetryInterval` = 200ms | settling 失败              |
| 设备丢失（不变）| `kMaxErrorRestarts` = 3 / `kRetryWindow` = 10s  | 流活过了 settle 窗口，或居然还在跑 |

- settle 失败 **绝不**触碰设备丢失预算。`kRouteSettleWindow` = 400ms 的取值依据：一次 A2DP ↔ 扬声器的 AudioPolicy
  转换在数百毫秒量级（同一份日志里一次成功的切换耗时 278ms），400ms 足以覆盖"刚起来就死"， 又不会把真实的设备丢失误判成抖动。
- **节流是"推迟，不升级"**：命中 `kSettleRetryInterval` 时 `restart_on_error()` **不改任何状态**直接返回错误。
  `ClientRuntime` 既有的 500ms supervision tick 随后经它 **既有的** silent-death 分支（`state == Running && !is_running()`）
  重新驱动恢复。这是刻意设计——ioc 线程上没有任何 sleep，UDP 心跳不会被饿死。
- settle 预算耗尽同样落 `Fatal`（`SwitchOutcome::Fatal` + `DeviceDisconnected`）。
- 用户显式 `set_playback_device()` 重置 **两套**预算（§5）。
- `CaptureManager` **刻意不设** settle 预算：这套机制是为 Android 的异步 AudioPolicy 重路由而生的，而 Android capture
  未实现，WASAPI 的设备失效也没有对应的窗口期。加一套用不上的机制只是死重。
