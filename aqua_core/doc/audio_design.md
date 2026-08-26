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

### 2.4 帧 `AudioFrame`

非拥有视图 block：`{ sequence, timestamp_ns, frame_count, data }`。采集、网络 payload 共用这一个类型，也是 JitterBuffer 的**输入 / slot 单位**（JB 把 `data` 拷贝进定长槽、按 `sequence` 排序）。JB 的消费输出直接向回放后端 `output` 填 PCM（缺帧填静音），不复用 `AudioFrame`；PLC 后续在消费侧无侵入接入。详细契约见 `buffer_design.md`。

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

- **采集（push）**：`AudioCapture::start(config, frame_cb, event_cb = {})`。
    - 帧回调运行在实时线程；`frame.data` 仅在回调内有效。
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

## 6. 分层与 sequence

- audio 层只关心 audio 层的数据，网络层只关心网络层的数据，各自有各自的 sequence。
- `AudioFrame::sequence` 是 audio 层的单调序号；网络包的乱序重排序号由网络层（分包器）负责。
- 具体边界（网络 sequence 放哪、jitter buffer 如何拿、水位 / 启动 / 并发契约）见 `buffer_design.md`。

## 7. 工厂模式

每个接口头即工厂（无独立 backend 总入口）：

- `audio_device_manager.h` → `create_device_manager()`（`aqua_core_base`）
- `audio_capture.h` → `create_capture(manager&)`（`aqua_server_core`）
- `audio_playback.h` → `create_playback(manager&)`（`aqua_client_core`）

后端实现头位于 `src/audio/<模块>/<backend>/`，仅被对应工厂 `.cpp` 引用。

## 8. 跨平台策略

| 能力                    | Windows         | Linux           | Android                      |
|-------------------------|-----------------|-----------------|------------------------------|
| 采集 INPUT（麦克风）    | WASAPI          | PipeWire / ALSA | AAudio                       |
| 采集 OUTPUT（loopback） | WASAPI loopback | monitor source  | **不支持（`NotSupported`）** |
| 回放 OUTPUT             | WASAPI          | PipeWire / ALSA | AAudio / OpenSL ES           |

平台能力差异通过错误码表达：`NotSupported`（方向 / 模式不支持）、`PermissionDenied`（麦克风权限）。

## 9. 已确定但暂缓

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
