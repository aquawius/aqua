# Aqua 音频模块设计

> 记录 aqua_core 音频子系统 **已确定**的设计决策，作为实现与后续讨论的依据。
> 状态：Windows 已完成 WASAPI Device / Capture / Playback；接收端 JitterBuffer 已按 `buffer_design.md` 实现（单缓冲、序列驱动、Fill/Drop 水位控制，含单元测试）；Linux / macOS / Android 待实现。

## 1. 定位与整体结构

Aqua 做的是"音频流实时共享"：在一台设备上采集音频，经网络传输，在另一台设备上回放。

- **两个程序，不是同一个程序**：
    - 采集程序（server 侧，`aqua_server_core` + `aqua_server_cli`）：采集 → UDP 广播。
    - 回放程序（client 侧，`aqua_client_core` + `aqua_client_cli`）：接收 → 回放。
- **CMake 按文件隔离**：采集文件只进 server core，回放文件只进 client core；设备枚举两端共用，放 `aqua_core_base`。
- **控制面**：gRPC（session 生命周期）； **数据面**：UDP（音频 + HELLO 保活）。

## 2. 核心域模型

### 2.1 方向 `AudioDeviceDirection`

`INPUT` / `OUTPUT`（`NONE` 仅作未初始化哨兵）。方向是设备查询、采集、回放共同的第一坐标轴。

### 2.2 设备 `AudioDevice` / `AudioDeviceId`

- `AudioDeviceId`：不透明字符串。只保证在当前 backend 适用范围内可作为设备身份做相等比较（`resolve(specific_id)` 依赖此比较）；不解析其内部结构，也不假定跨会话 / 跨平台 / 跨机器稳定。
- `AudioDevice`：`{ id, name, direction, is_default }`，值语义。
    - 设备本身不携带 `AudioFormat`。Format 属于具体 AudioStream，而不是设备。

### 2.3 格式 `AudioFormat`

PCM 描述（`encoding` / `channels` / `sample_rate`），与 proto `AudioFormat` 一一对应。`is_valid()` 判定合法性。

### 2.4 数据块与定长帧（三层类型边界）

- **数据块 `AudioBlock`**：采集后端产出的原始 PCM 块（`{ data }`），块大小**不固定**（如 WASAPI 一次事件含多个 packet）。仅采集侧使用，不含 sequence。
- **定长帧 `AudioFrame`**：由 `AudioBlock` 重切而成的固定大小帧（`{ sequence, frame_count, data }`），`frame_count` 即 `F`，session 内固定。由 server 侧 `AudioPacketizer` **唯一产生 sequence**，是 JitterBuffer 的**输入 / slot 单位**（JB 把 `data` 拷贝进定长槽、按 `sequence` 排序）。JB 的消费输出直接向回放后端 `output` 填 PCM（缺帧填静音），不复用 `AudioFrame`；PLC 后续在消费侧无侵入接入。
- **网络帧 `NetworkFrame`**：`AudioFrame` 打上网络包头（type + sequence 序列化 + payload）后的 wire 帧，覆盖 Audio / Hello / HelloAck 全部 UDP 数据报（见 `network_frame.h`）。

详细契约见 `buffer_design.md`。

### 2.5 采集信息 `AudioCaptureInfo`

`AudioCaptureInfo` 描述已经创建的 AudioCapture stream 的实际属性，其中 `format` 是该 stream 的权威格式。`AudioCaptureConfig::format == nullopt` 时，由 backend 根据实际平台的 shared-mode / negotiated format 决定并通过 `AudioCapture::info()` 暴露。

## 3. 设备选择与默认语义

统一用 `AudioDeviceDirection + std::optional<AudioDeviceId>` 表达：

| 场景                 | direction     | device      |
|----------------------|---------------|-------------|
| 默认麦克风           | INPUT         | nullopt     |
| 指定麦克风           | INPUT         | 麦克风 id   |
| 默认混音（loopback） | OUTPUT        | nullopt     |
| 指定输出设备的混音   | OUTPUT        | 输出设备 id |
| 回放默认             | （恒 OUTPUT） | nullopt     |
| 回放指定             | （恒 OUTPUT） | 输出设备 id |

- `nullopt` = 该方向的系统默认设备（在 `start()` 时解析）。
- 有值 = 指定设备，其 direction 必须与配置方向一致。
- **loopback 不是设备**，它是"采集 OUTPUT 方向的系统混音"：采集 `direction == OUTPUT` 即 loopback。

设备查询接口 `AudioDeviceManager`：

- `enumerate(direction)`：列设备（UI 用）。
- `default_device(direction)`：取默认（可读性便利）。
- `resolve(direction, requested)`：`nullopt→默认` / `id→指定`（校验方向），是启动路径；失败通过 `std::expected<AudioDevice, AudioError>` 区分设备不存在与 backend 失败。
- （已去掉 `find(id)`：唯一方向无关的查询，容易引发方向错配。）

## 4. 采集 / 回放接口

- **采集（push）**：`AudioCapture::start(config, block_cb, event_cb = {})`。
    - 块回调运行在实时线程，投递变长 `AudioBlock`；`block.data` 仅在回调内有效。
    - `event_callback` 投递运行期错误（`DeviceDisconnected` 等），在 backend 内部线程（非实时数据路径）。
- **回放（pull）**：`AudioPlayback::start(config, cb, event_cb = {})`。
    - 回调返回实际填充帧数；未填满部分后端补静音。
    - 同样有 `event_callback`。
- 回调统一为 `std::move_only_function<... noexcept>`（C++23），状态经 lambda capture 传入，
  不再有 `user_data` 参数；`std::move_only_function` 的 noexcept 签名在编译期强制回调不抛异常。
- 生命周期：`stop()` 后保证回调不再被调用；可再次 `start()`；不得从回调内调用 `stop()` / `start()`。

## 5. 格式契约（关键）

- capture 不做转换：`config.format` 指定时，严格交付该格式；backend 原生不支持则 `FormatUnsupported`。
- `config.format == nullopt` 时，由 backend 选择该 stream 的默认/shared-mode 格式，并在 `AudioCaptureInfo::format` 中报告。
- playback 不做转换：按 `config.format` 填充 output；设备不支持则 `FormatUnsupported`。
- 转换（重采样 / 位深 / 声道）由 client 侧、在喂给回放回调之前完成；做不做转换另行决定。
- server 创建 capture stream 后，以 `AudioCaptureInfo::format` 作为该 stream 的权威格式，通过 gRPC 下发给 client；AudioDevice 不再承担 Format 来源职责。

## 6. Server 实时路径边界

Server capture callback 是严格 realtime 路径：只允许 `AudioPacketizer` 的有界拷贝、
`AudioFrameQueue` 的 SPSC 入队以及独立 worker 的原子唤醒。以下工作不得在
capture callback 执行：wire encoding、`SessionManager` 加锁/快照、`shared_ptr` 广播、
UDP enqueue、Asio executor submission。

数据流固定为：

```text
WASAPI Capture RT
    ↓
AudioPacketizer
    ↓
AudioFrameQueue（仅 RT→network handoff，非播放缓冲）
    ↓
AudioNetworkDispatcher（唯一执行 AudioFrame → NetworkFrame）
    ↓
UdpServer（session-aware datagram fan-out）
    ↓
UdpTransport
```

`AudioFrameQueue` 默认仅 4 个 slot；以默认约 3 ms/frame 计算，最多只允许约 12 ms 的 handoff backlog。
它的目标是隔离 scheduler 短抖动，而不是吸收长期网络拥塞。队列满时丢最新帧，避免通过增长 backlog 把实时性换成延迟。
一旦拥塞持续，网络端观测到的是 sequence gap，由客户端 JitterBuffer 按正常丢包路径处理。

### 6.1 AudioNetworkDispatcher 并发模型

`AudioNetworkDispatcher` 是 capture RT 与 UDP 数据面之间的专职网络线程：

- **独立 `std::thread`**：drain `AudioFrameQueue` → `NetworkFrame` encode → `UdpServer::broadcast`。
  不放在 asio `io_context` 上，因为唤醒它必须由 capture RT 触发，而 RT 禁止 `asio::post`
  （会分配 handler + 抢 io_context 内部队列锁）。
- **唤醒协议**：`std::atomic<std::uint64_t> wake_generation_` + `wait/notify_one`。
  producer 每次成功 `push()` 都执行一次 `wake_generation_.fetch_add(1)`；`push()` 发布 slot 后重新
  读取 consumer cursor 计算 `should_notify`，仅当本次 push 仍可能是第一个待处理项时才执行 `notify_one()`。
  `should_notify` 只是 producer 的唤醒提示，不是 queue 当前状态的同步事实。generation 用来关闭 worker 的 `load(observed) → wait(observed)` 竞态：
  如果 push 发生在 load 与 wait 之间，generation 已变化，`wait(observed)` 不会真正睡眠；如果
  worker 已经阻塞在 wait，则 empty→non-empty 对应的 notify_one() 负责将其唤醒。
  因而 correctness 依赖 generation + 正确的双重队列检查，而不是 notify 的“投递时机”。
- **drain 语义**：`run()` 退出循环后仍执行一次最终 `drain()`，因此 `stop()` 会把 stop 前已入队、
  尚未发送的帧全部编码广播，不遗留在半途。
- **实时路径约束**：worker 上的 encode（堆分配）、`SessionManager` snapshot（shared lock）、
  `UdpTransport` 入队（mutex）都不在 capture RT 线程上，符合 §6 的边界。

### 6.2 Runtime lifecycle

ServerRuntime 与 ClientRuntime 都采用一次性生命周期：
`Created → Starting → Running/Degraded → Stopping → Stopped`。`Degraded` 是终态：发生运行期音频后端错误后不自动回到 `Running`，当前版本由 owner 决定何时 stop；错误码保留在 runtime diagnostics 中。`start()` 与 `stop()` 属于 control-plane 操作，不要求并发安全；`stop()` 本身通过 CAS 抢占 `Stopping`，因此重复/并发 stop 调用最多只有一个线程执行 teardown。

`AudioCapture` / `AudioPlayback` 的运行期 event callback 只记录错误并把 runtime 推进到 `Degraded`，绝不在 backend event thread 中调用 `stop()`；真正 teardown 仍由控制线程执行。

Server 的 reap timer 使用 `weak_ptr` 捕获，避免 timer 回调形成 Runtime 自持有环。UDP Server/Client 的异步协议与 HELLO timer 同样使用 weak capture，因而即使 `io_context` 已停止、清理 handler 没有机会执行，也不会产生 `State → handler → State` 的永久引用环。


## 7. 分层与 sequence

- audio 层与网络层拥有明确的数据类型边界，但音频 sequence 在进入 wire 后沿用同一个值，不再重新编号。
- `AudioFrame::sequence` 是 audio 数据面的单调序号，由 `AudioPacketizer` 唯一产生；`AudioBlock`（采集侧）不含 sequence。`NetworkFrame::Audio` 携带同一 sequence，客户端 JitterBuffer 按它完成乱序/缺帧处理。
- 具体边界（网络 sequence 放哪、jitter buffer 如何拿、水位 / 启动 / 并发契约）见 `buffer_design.md`。

## 8. 工厂模式

每个接口头即工厂（无独立 backend 总入口）：

- `audio_device_manager.h` → `create_device_manager()`（`aqua_core_base`）
- `audio_capture.h` → `create_capture(manager&)`（`aqua_server_core`）
- `audio_playback.h` → `create_playback(manager&)`（`aqua_client_core`）

后端实现头位于 `src/audio/<模块>/<backend>/`，仅被对应工厂 `.cpp` 引用。

## 9. 跨平台策略

| 能力                    | Windows         | Linux           | Android                      |
|-------------------------|-----------------|-----------------|------------------------------|
| 采集 INPUT（麦克风）    | WASAPI          | PipeWire / ALSA | AAudio                       |
| 采集 OUTPUT（loopback） | WASAPI loopback | monitor source  | **不支持（`NotSupported`）** |
| 回放 OUTPUT             | WASAPI          | PipeWire / ALSA | AAudio / OpenSL ES           |

平台能力差异通过错误码表达：`NotSupported`（方向 / 模式不支持）、`PermissionDenied`（麦克风权限）。

## 10. 已确定但暂缓

- **默认设备中途变更跟随**：v1 只在 `start()` 时解析一次默认；运行中默认切换不跟随（仅设备失效触发 `DeviceDisconnected`
  ）。自动跟随留待后续。

## WASAPI implementation notes

- COM 初始化是线程级状态，不是进程级开关。DeviceManager 在调用线程初始化 COM；WASAPI Capture 的 realtime audio thread 自己初始化 COM。
- 两者都使用 `COINIT_MULTITHREADED`。这不是重复初始化同一个 apartment，而是分别初始化两个线程自己的 COM apartment。
- `RPC_E_CHANGED_MODE` 表示调用线程已经进入另一种 COM apartment；现有 COM 环境仍可使用，但 helper 不调用 `CoUninitialize()`。
- `src/audio/wasapi/wasapi_com.h` 是 Audio backend 内部共享 RAII helper，公共 API 不暴露 COM。
- WASAPI Capture 使用 Shared Mode + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`；Loopback 额外使用 `AUDCLNT_STREAMFLAGS_LOOPBACK`。
- WASAPI Playback 优先使用 `IAudioClient3::InitializeSharedAudioStream`，通过 `GetSharedModeEnginePeriod` 选择合法的低延迟周期；如果 `IAudioClient3` 不可用或初始化失败，则回退到 `IAudioClient::Initialize` 的 Shared + Event 模式。
- Playback 的共享模式事件驱动路径使用 `hnsBufferDuration = 0` / `hnsPeriodicity = 0`，由 WASAPI 音频引擎周期决定实际 buffer。
- Playback / Capture 的 realtime thread 均使用 MMCSS `Pro Audio`；应用回调不在控制线程执行。
- Capture 的 realtime thread 保持在 WASAPI backend 内部；Playback 同理。ServerRuntime / ClientRuntime 只负责未来的业务编排，不直接拥有 WASAPI realtime loop，以避免 runtime 反向依赖平台 backend。

### 6.3 Network dispatch accounting

Server 诊断将网络 worker 的每帧结果分开统计：`frames_encoded` 表示 wire encode 成功；`frames_broadcast` 表示存在至少一个已连接 session 并进入广播路径；`frames_without_clients` 表示 encode 成功但当时没有连接 session；`encode_failures` 表示 wire encode 失败；`dispatch_failures` 表示 broadcast snapshot/dispatch 自身失败。UDP 实际发送结果由 `UdpTransportStats.tx_*` 单独统计，不与上述 application-level accounting 混淆。


## Client 网络活性与两级恢复

Client UDP HELLO 周期同时承担 session keepalive 与客户端侧 liveness 观察。Server 对 HELLO 返回 HELLO_ACK；Client 按 HELLO interval 检查是否出现新的 ACK generation。连续 `HELLO_ACK_MISS_THRESHOLD` 个 interval 未观察到新 ACK 时，`UdpClient` 触发 liveness callback，由 `ClientRuntime` 将状态从 `Starting/Running` 锁存为 `Degraded`。单次 ACK 恢复会清零连续 miss 计数；ACK age 与 miss count 仅用于 diagnostics。

网络活性与 JitterBuffer timeline recovery 是两个独立层级：

- Level 1：session 仍活跃，但 AudioFrame sequence 脱离当前 JB playback window → JitterBuffer re-anchor。
- Level 2：HELLO_ACK 连续超时或重新建立 session → Runtime 进入/重建 session 级状态，旧 JB 时间轴不得跨 session 复用。

“没有 AudioFrame”本身不被视为网络断连，因为远端可能合法地处于静音状态。
### Runtime failure reconciliation

Capture、playback 和 UDP liveness 线程只报告故障；CLI/control thread 以 500 ms 周期观察 RuntimeState。
发现终态 `Degraded` 后执行统一 stop 流程。500 ms 周期不是音频/网络计时器。

