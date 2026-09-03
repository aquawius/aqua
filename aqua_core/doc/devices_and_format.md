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

设备解析后，Runtime 在 start 路径冻结本次 run 所用 endpoint（构造期解析仅用于格式探测，确定 packetizer/queue 几何）。系统默认设备运行期间发生变化不会静默切换当前 stream 的几何；设备故障/默认变化由 CaptureManager（server）/ PlaybackManager（client）按候选链重建端点（见 `capture_switching_design.md` / `playback_switching_design.md`），会话格式不变。

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
