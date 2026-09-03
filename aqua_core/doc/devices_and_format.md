# 设备与格式

## 1. AudioDevice 是值对象

`AudioDevice` 不持有 OS stream。字段：

```text
AudioDeviceId id
string name
Direction INPUT/OUTPUT
bool is_default
```

`AudioDeviceId` 是平台不透明字符串：同一平台 session 内通常可稳定用于比较/缓存，但不能跨机器、跨平台假设稳定。

## 2. AudioDeviceManager

职责只有：

- enumerate (direction)
- default_device (direction)
- default_format (direction, requested)
- resolve (direction, requested)

它不创建 AudioCapture/AudioPlayback stream，也不参与实时线程。

## 3. WASAPI

当前 Windows backend 对应：

```text
WasapiAudioDeviceManager
WasapiAudioCapture
WasapiAudioPlayback
```

设备解析分两处，作用不同：

- ServerRuntime 在构造期解析一次，只用于探测格式（确定 packetizer / queue 几何）；
- 采集/回放的启动与切换由 `CaptureManager` / `PlaybackManager` 按路由重新解析。

因此系统默认设备在运行期间变化**不会**静默改变当前 stream 的几何——会话格式是常量。设备故障或默认设备变化走候选链重建
端点（见 `capture_switching_design.md` / `playback_switching_design.md`），会话与格式都不受影响。

## 4. Loopback

`OUTPUT_LOOPBACK` 是 capture source，不是一个独立的 AudioDevice 类型。解析方向仍然是 OUTPUT。

因此 server 默认：

```text
source = OUTPUT_LOOPBACK
device = default OUTPUT
```

## 5. 平台限制

当前源码 factory 提供 Windows/WASAPI 全量实现（capture + playback）与 Android/AAudio playback（含最小
DeviceManager：默认输出路由，显式设备 ID 不支持）。Linux、macOS 的后端尚未实现。Android capture（含
OUTPUT_LOOPBACK）未实现——系统 API 不提供 loopback，返回 NotSupported。

## 6. 格式支持策略

Core 不要求 backend 支持所有 `AudioEncoding`。backend 可以拒绝格式，统一返回 `FormatUnsupported`。Client 不应尝试“差不多的格式”或静默
resample；Server 的 ConnectResponse 格式就是本次 session 的数据契约。
