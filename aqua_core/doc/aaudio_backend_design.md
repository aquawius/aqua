# AAudio Backend 设计决议

本文冻结 Android 音频后端的格式协商、低延迟与设备路由方案。 讨论时间：2026-08-31；实施前最终决议，实现不得偏离本文。 背景与里程碑计划见
`android_roadmap.md`。

## 1. Playback 格式协商：宽松接受，字节契约硬校验

Playback 是数据汇（音频汇），核心原则： **能播就行，系统内转换交给系统**。

### 1.1 协商流程

```text
server 契约格式 (gRPC ConnectResponse: encoding + channels + sample_rate)
    ↓ 全量作为 AAudioStreamBuilder 请求参数
open stream
    ↓ 回读实际 stream 配置
校验（唯一硬约束）:
    encoding  == 契约 encoding     （字节布局，错则出噪音）
    channels  == 契约 channels      （声道 remix 有串音风险）
    sample_rate 允许系统 SRC        （AAudio/MixerBox 内部重采样）
    ↓ 任一硬约束不满足
返回 FormatUnsupported，ClientRuntime 拒绝启动 playback
```

### 1.2 为什么采样率可以放

- JitterBuffer pull 的是契约采样率的固定数据；AAudio 内部 SRC 只影响系统向 DAC 喂数的节奏，不改变 JitterBuffer 的时钟基准；
- 采样率漂移被 JitterBuffer 既有水位机制自然吸收（高水位 Drop / 低水位 Fill）， 这本来就是它的职责；
- 代价：44.1k 契约流在 48k-only 设备上多几毫秒系统内缓冲延迟，可接受。

### 1.3 为什么编码/声道不能放

`AAudioStream` 数据回调的字节流必须按契约格式解释，否则 PCM 位错乱直接 出噪音——这是字节布局问题，系统转换无法替我们兜底。声道数不一致
（如请求 2ch 回读 1ch）同样拒绝：声道 remix 语义不受控。

### 1.4 与 WASAPI 的差异说明

WASAPI playback 使用 `IsFormatSupported` 预检，编码/声道/采样率三者均要求 严格一致；AAudio backend
对采样率放宽（理由如上）。两端对"编码+声道"的 严格度一致。此差异为有意设计，不是疏漏。

## 2. 低延迟参数（已冻结）

| 参数              | 取值                                          | 理由                                                                                      |
|-------------------|-----------------------------------------------|-------------------------------------------------------------------------------------------|
| PerformanceMode   | 由播放配置选择 `NONE` / `LOW_LATENCY`         | Android 设置「低延迟模式」控制；两者均使用 `SHARED`，不启用 Exclusive                     |
| SharingMode       | `AAUDIO_PERFORMANCE_MODE_SHARING_MODE_SHARED` | Exclusive 不做（与 Windows 一致）                                                         |
| framesPerCallback | 0（自适应）                                   | 回调粒度 = 设备原生 burst，JitterBuffer pre-roll 水位计算最准；固定值在部分设备触发双缓冲 |
| buffer 大小       | 不显式设置                                    | 保持 AAudio 后端自适应；不因低延迟开关改变显式 buffer 容量策略                          |
| Usage             | `AAUDIO_USAGE_MEDIA`                          | 表达媒体播放意图，交系统路由                                                              |

延迟大头不在 AAudio：JitterBuffer 深度（默认 30 slots ≈ 90ms@48k）是网络 抖动吸收垫，将来 UI 可暴露调节；蓝牙路由（SBC/AAC 编码
100-200ms）为协议 层固有，AAudio 无法改善，UI 层提示即可。

## 3. 设备路由：跟随系统，不做枚举

### 3.1 Android 的限制（事实边界）

- AAudio 无 `IMMDeviceEnumerator` 等价物；`setDeviceId` 的 id 来自 Java 层
  `AudioManager.getDevices()`，仅对 USB/BT 外接设备可靠，对内建设备行为 未定义，官方不推荐；
- Android 音频路由由 AudioPolicy 集中决策（插耳机自动切、来电抢占、蓝牙 SCO 接管），应用表达"意图"（usage），不指定"设备"；
- 路由变化通过 stream 的 error/disconnect 回调感知，正确响应是重建流， 而非切换设备。

### 3.2 DeviceManager 实现（playback 阶段落地）

```text
enumerate(OUTPUT)          → 单条合成条目「System Default Output」
default_device(OUTPUT)     → 同上
resolve(OUTPUT, nullopt)   → 「System Default Output」   （唯一正路）
resolve(OUTPUT, 有值)      → DeviceNotFound               （拒绝显式选择）
default_format(OUTPUT, nullopt) → client 路径不调用（格式来自 gRPC 契约）
```

### 3.3 设备切换 = stop → start

ClientRuntime 生命周期本为一次性。拔插耳机：系统重路由 → 流断 →
`DeviceDisconnected` → runtime Degraded → C API 监督线程 stop → Kotlin 层 重建。全程复用现有状态机，零新增代码。

### 3.4 用户可见行为对照

| 场景          | Windows            | Android                            |
|---------------|--------------------|------------------------------------|
| 选输出设备    | UI 列出 endpoint   | 无 UI，跟随系统（控制中心切）      |
| 选输入设备    | UI 列出 endpoint   | 默认麦克风；后续可加 BT/USB 麦选项 |
| 拔插设备      | DeviceDisconnected | 同左，重路由后重建流               |
| 内录 loopback | 支持               | 不支持（见 §5）                    |

## 4. Capture（后续阶段，接口预留冻结）

### 4.1 格式协商方向与 playback 相反

```text
Playback:  server 契约 → 请求 → 系统尽力 → 能播就行（宽）
Capture:   设备实际格式 → 如实上报为 server 契约 → 全体 client 服从（严）
```

- Android 输入流实际格式由设备决定（native 率可能是 16k/44.1k/48k，声道 1/2，格式 I16）， **必须回读实际格式并如实上报**为
  server 运行时契约 （同 WASAPI capture 在 `config.format == nullopt` 时的行为）；
- server 侧 packetizer/JitterBuffer/MTU 几何全部从实际格式推导，AAudio 内部 SRC 会导致时钟与字节对不上——capture
  不接受任何隐式系统转换。

### 4.2 第一阶段范围

- 实现：麦克风输入（`source == INPUT`，默认输入设备，`resolve(nullopt)`）；
- 权限：`RECORD_AUDIO` 运行时权限由 Kotlin 层在启用 server 模式时请求， native 侧缺失时返回 `PermissionDenied`（
  `AudioError` 枚举已预留）；
- 显式输入设备选择：后续通过 Java 层 `AudioManager.getDevices()` 传 id 至 native `resolve(id)` → `setDeviceId`，这是
  Android 唯一受支持的显式 选择路径。

### 4.3 内录（OUTPUT_LOOPBACK）接口预留

- AAudio 无 loopback 输入预设，系统级不支持 native 内录；
- Android 内录的正路是 Java 层 `AudioPlaybackCapture`（API 29+，要求 foreground service + 用户授权），与 AAudio capture
  是两条完全不同的 技术路径；
- **接口策略**：`AudioCaptureSource::OUTPUT_LOOPBACK` 在 Android capture backend 实现时返回 `NotSupported`
  （枚举已预留），保持上层调用面完整； 内录后续作为独立特性立项，基于 `AudioPlaybackCapture` 实现，必要时 引入 Java 层捕获管道喂给
  native capture 抽象。

## 5. 错误处理与回调契约（playback/capture 共用）

AAudio 硬约束： **`AAudioStream_close` 不得在 data callback 内调用**（死锁）。 处理模式：

1. data callback 遇运行期错误 → 返回 `AAUDIO_CALLBACK_RESULT_STOP`（停止 数据分发，不关流）；
2. error callback 发布 pending error（原子存储，供 data callback 观察后 STOP），不在回调内 close/stop；同时**即时投递 event callback**
   （一次性，`report_fatal_once`）——与 WASAPI "event thread 处理运行期错误"
   对等。修订记录：早期版本只在 `stop()` 投递 pending error，导致流死后
   runtime 无从感知（JB 打满、永久静音）；运行期错误必须在发生时就进入
   ClientRuntime 的错误驱动恢复；
3. 真正的 close/restart 由控制线程的 `stop()` 执行；`stop()` 对尚未即时
   投递的 pending error 做兜底投递（已投递的不重复）。

RT 回调契约与 WASAPI 完全一致：不加锁、不分配、不做 IO、不调用 stop/close、必须填满 output、返回实际帧数（见 `audio_playback.h`
头注释）。

## 6. 其他冻结项

- **STL**：`c++_shared` 随 APK 打包（NDK sysroot 拷贝）；将来引入任何第三方 native 库必须共用同一 STL，禁止静态链第二份
  libc++；
- **日志**：Android 默认 Info 级（logcat Debug 刷屏且被 `isLoggable`
  过滤）；C API `log_level` 字段透传，Kotlin 设置页将来可调；
- **MMAP**：AAudio 的具体底层路径由系统/设备决定；本分支只控制 `NONE` / `LOW_LATENCY` performance mode，始终使用 `SHARED`，真机验证时需注意 OEM
  差异。

## 7. 实施顺序（本文冻结后的落地步骤）

1. `AAudioAudioPlayback`：§1 协商 + §2 参数 + §5 错误模式；
2. Android `AudioDeviceManager` 最小实现（§3.2）；
3. 两个 factory + `aqua_core/CMakeLists.txt` 的 `ANDROID` 门控接线；
4. android-arm64 preset 编译验证 + Windows 零回归；
5. 真机链路验证（Wi-Fi → gRPC → HELLO/ACK → pre-roll → 出声）。

capture 侧（§4）不写代码，全部依赖现有抽象的既有兜底 （`DeviceNotFound` / `PermissionDenied` / `NotSupported`），无预留改动。
