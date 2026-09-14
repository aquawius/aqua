# 模块：WASAPI 后端

Windows 上采集与回放各有一个 backend 实现。两者结构对称：自己管理线程与同步对象，Runtime 只见抽象接口。

## 1. 线程与句柄

```text
audio_thread   实时音频线程（采集：事件等待 + drain packet；回放：提交 buffer）
event_thread   运行期错误事件线程
stop_event     stop 请求（manual reset）
audio_event    engine 数据就绪信号
error_event    错误事件信号
running flag   运行标志
```

控制线程调用 `start()` / `stop()`。两个工作线程都是 `std::jthread`，停止握手仍是 Win32 事件：`stop()` 调
`request_stop()`，由线程体内注册的 `stop_callback` 执行 `SetEvent(stop_event_)`（event 线程对应
`SetEvent(error_event_)`），再 join。`running_` 只是状态旗（`is_running()` 与错误派发顺序用），不是停止机制。

**禁止在 block / event 回调内调用 `stop()`**：虽然 `stop()` 会显式跳过对自身的 join，但句柄与回调的回收会被推迟，
且自连接路径下本次调用形同"只置停止"。

## 2. Playback callback

WASAPI 在需要输出时调用 Core 的 pull 回调：

```text
ClientRuntime::pull_playback()
        ↓
JitterBuffer::pull()
```

中间没有额外 ring buffer。

## 3. Capture

`WasapiAudioCapture` 以事件驱动方式读取 endpoint buffer（`WaitForMultipleObjects`，20ms 有界等待）。一次事件可产生多个
packet，因此交给 Core 的 `AudioBlock` 是变长的，由 `AudioPacketizer` 重切成定长帧。

## 4. Loopback 时间轴补偿（欠账驱动）

loopback 在 quiescence 期间不产生 audio event，切歌等场景还会出现空事件（signal 但不产包）或部分饥饿。当前实现不靠"超时
且零包"这种窄条件，而是每轮唤醒统一对账：

```text
expected  = 距上轮结算的墙钟欠账（小数累积，防漂移）
balance  += expected - 本轮真实交付帧数
balance > 0  合成静音补齐
balance < 0  记为盈余，抵扣后续欠账
```

单轮补偿上限 150ms（兼作盈余留存上限）。流级诊断状态连续 2 轮补偿后才置 `Starved`。细节见 `capture.md`。

## 5. 设备生命周期

backend 不"自动换设备"：它只保证同一实例 `stop()` 后可以再次 `start()`（端点重建是 `CaptureManager` / `PlaybackManager`
的事务）。运行期设备失效经 event 回调上报，映射规则为：

```text
AUDCLNT_E_DEVICE_INVALIDATED / AUDCLNT_E_RESOURCES_INVALIDATED -> DeviceDisconnected
AUDCLNT_E_SERVICE_NOT_RUNNING                                  -> BackendFailed
AUDCLNT_E_UNSUPPORTED_FORMAT（启动期）                          -> FormatUnsupported
AUDCLNT_E_DEVICE_IN_USE / AUDCLNT_E_ENDPOINT_CREATE_FAILED      -> DeviceUnavailable
E_ACCESSDENIED                                                  -> PermissionDenied
```

请求显式格式时，启动路径先 `IsFormatSupported()` 校验；未指定格式则用设备 mix format，并在 `info()` 中报告实际格式。

## 6. Exclusive

Aqua 当前产品契约不采用 WASAPI Exclusive。任何试图以独占模式重新设计 buffer/clock 的实现都不属于当前架构。
