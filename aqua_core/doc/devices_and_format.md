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

- enumerate(direction)
- default_device(direction)
- default_format(direction, requested)
- resolve(direction, requested)

它不创建 AudioCapture/AudioPlayback stream，也不参与实时线程。

## 3. WASAPI

当前 Windows backend 对应：

```text
WasapiAudioDeviceManager
WasapiAudioCapture
WasapiAudioPlayback
```

设备解析后，Runtime 在 start 路径冻结本次 run 所用 endpoint。系统默认设备运行期间发生变化不会静默切换当前 stream；需要 stop/restart 才重新 resolve。

## 4. Loopback

`OUTPUT_LOOPBACK` 是 capture source，不是一个独立的 AudioDevice 类型。解析方向仍然是 OUTPUT。

因此 server 默认：

```text
source = OUTPUT_LOOPBACK
device = default OUTPUT
```

## 5. 平台限制

当前源码 factory 只提供 Windows/WASAPI 实现。Linux、macOS、Android 的 AudioDeviceManager/Capture/Playback 尚未进入当前 Core 实现；Android roadmap 会以同一抽象接入 AAudio。

## 6. 格式支持策略

Core 不要求 backend 支持所有 `AudioEncoding`。backend 可以拒绝格式，统一返回 `FormatUnsupported`。Client 不应尝试“差不多的格式”或静默 resample；Server 的 ConnectResponse 格式就是本次 session 的数据契约。
