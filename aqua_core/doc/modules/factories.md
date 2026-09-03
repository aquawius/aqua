# 模块：Platform Factories

## 三个工厂

```cpp
create_device_manager()                       // aqua/audio/devices/audio_device_manager.h
create_capture(AudioDeviceManager&)           // aqua/audio/capture/audio_capture.h
create_playback(AudioDeviceManager&)          // aqua/audio/playback/audio_playback.h
```

工厂是 Core 与平台 backend 的唯一选择点。

## 为什么返回 nullptr

Core 可以在缺少某个平台 backend 的情况下编译。例如非 Windows 构建不编入 WASAPI 实现，此时 `create_capture()` /
`create_playback()` 返回 `nullptr`。

Runtime 把这视为"当前平台能力不可用"，而不是在各业务模块里到处写平台判断。捕获到的 `nullptr` 会转成 `BackendFailed` 或
启动期错误，不进入运行路径。

## 当前映射

| 平台    | AudioDeviceManager        | AudioCapture        | AudioPlayback        |
|---------|---------------------------|---------------------|----------------------|
| Windows | `WasapiAudioDeviceManager`| `WasapiAudioCapture`| `WasapiAudioPlayback`|
| Android | `AAudioAudioDeviceManager`| 无实现              | `AAudioAudioPlayback`|
| 其它    | 无实现                    | 无实现              | 无实现               |

Android 的 `create_capture()` 返回 `nullptr`（`OUTPUT_LOOPBACK` 在 Android 属于后续阶段；core 侧契约已就位，接入即可获得
采集端切换能力）。

## 切换管理器的构造

`CaptureManager` / `PlaybackManager` 各自有两个构造入口：

```cpp
explicit CaptureManager(AudioDeviceManager&);                       // 生产：内部调 create_capture
explicit CaptureManager(std::unique_ptr<AudioCapture>,
                        AudioDeviceManager* device_manager = nullptr); // 测试：注入 mock 后端
```

工厂仍在管理器内部被调用——管理器只是把"选择哪个后端"与"如何编排切换"分层，没有绕过工厂。
