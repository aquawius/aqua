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

## Android 未来

Android 接入时只应增加：

```text
AudioDeviceManager -> Android/AAudio device backend
AudioPlayback      -> AAudio playback backend
```

不应修改 ClientRuntime 的网络/缓冲主流程。
