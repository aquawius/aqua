# 模块：Platform Factories

## 三个工厂

```text
create_device_manager()
create_capture(device_manager)
create_playback(device_manager)
```

工厂是 Core 与平台 backend 的唯一选择点。

## 为什么工厂返回 nullptr

当前 Core 可以在没有某个平台 backend 的情况下被编译。例如当前源码在非 Windows 上不会编入 WASAPI 实现，因此 factory 返回
nullptr。

Runtime 把这视为“当前平台能力不可用”，而不是在各业务模块里到处写平台判断。

## Windows

当前：

```text
AudioDeviceManager -> WasapiAudioDeviceManager
AudioCapture       -> WasapiAudioCapture
AudioPlayback      -> WasapiAudioPlayback
```

## Android

已按此模型接入（仅 playback）：

```text
AudioDeviceManager -> AAudioAudioDeviceManager（src/audio/devices/aaudio/）
AudioPlayback      -> AAudioAudioPlayback（src/audio/playback/aaudio/）
AudioCapture       -> 无实现（capture 为后续里程碑；OUTPUT_LOOPBACK 返回 NotSupported）
```

接入未修改 ClientRuntime 的网络/缓冲主流程——factory 是唯一选择点这一设计已被验证。
