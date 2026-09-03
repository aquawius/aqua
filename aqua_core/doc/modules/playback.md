# 模块：AudioPlayback / PlaybackManager / WASAPI / AAudio Playback

## 抽象

`AudioPlayback` 是输出 pull 接口：

```cpp
start(config, pull_callback, event_callback)
stop()
is_running()
stream_info()
```

pull 回调签名：

```text
span<byte> output -> 返回实际写入的 frames
```

backend 必须把未写入区域清零，并保证 `stop()` 返回后不再调用回调。与 `AudioCapture` 一样，控制 API 只能由控制线程调用，
禁止在回调内调用 `start()` / `stop()`。

## PlaybackManager

`PlaybackManager`（`src/audio/playback/playback_manager.cpp`）托管回放流生命周期与切换事务，对称 server 侧的
`CaptureManager`：

```text
ClientRuntime --> PlaybackManager --> AudioPlayback --> WASAPI / AAudio
```

- **路由模式**：`FollowSystem`（跟随系统默认输出）/ `PreferCurrent`（钉住首流实际设备，连接起步时设定）/ `PreferredDevice`
  （用户显式选择）。
- **候选链**：`[目标设备, 先前的实际设备, 系统默认]` 去重；成功后按落点给出 `Switched` / `RolledBack` /
  `FellBackToSystem`；链耗尽 = `Fatal`。
- **防抖**：错误驱动 restart 在 10s 窗口内最多 3 次；用户显式选择不计数并重置窗口。
- **驱动入口**：`restart()`（同设备重建）、`set_playback_device(target)`（显式选择）、`restart_on_error()`（错误驱动）、
  `tick()`（FollowSystem 轮询默认设备）、`on_devices_changed(ids)`（平台推送的设备快照，Android 走这条）。

完整决议见 `../playback_switching_design.md`。

## Client 接线

`ClientRuntime` 把 `ConnectResponse` 下发的格式强制写入配置：

```text
ConnectResponse.audio_format
        ↓
AudioPlaybackConfig.format
        ↓
PlaybackManager::start
```

后端不支持该格式即启动失败，不会尝试"接近格式"。

启动阶段还有一次**设备兜底**：若带 `--device-id` 的首次 `start()` 失败，会以系统默认设备重试一次并记日志，避免单个设备不可
用直接导致连接失败。

## WASAPI 当前模型

`WasapiAudioPlayback` 自己拥有 audio thread 与 event thread：audio thread 负责实时提交与
`GetCurrentPadding` / `GetBuffer` / `ReleaseBuffer`；event thread 负责运行期错误事件。Runtime 不直接触碰 WASAPI COM 对象。

## AAudio 当前模型（Android）

`AAudioAudioPlayback`（`src/audio/playback/aaudio/`）遵守同一 pull 抽象与 RT 契约：

```text
performance   NONE / LOW_LATENCY（由 Android「低延迟模式」设置选择）
sharing       SHARED（不做 Exclusive）
data callback (audioData, numFrames) -> span<byte> -> ClientRuntime::pull_playback
error callback 只发布 pending_error_，不 close / stop
stop          只由控制线程执行：requestStop + close（close 等待在途回调返回）
```

格式策略（决议见 `../aaudio_backend_design.md`）：encoding 与 channels 必须与 server 契约一致；采样率允许系统重采样（回读
实际 stream 配置校验通道/编码）；`framesPerCallback = 0` 自适应设备 burst。回调上下文经 `shared_ptr` 保活，close 与在途回调
竞争时对象不失效。

## 不支持 Exclusive

Aqua 的 playback 产品契约不使用 Windows Exclusive。不要为降低几毫秒在 backend 中引入第二套共享模式/时钟模型——那会改变
现有的回调与生命周期假设。

## 与 JitterBuffer 的关系

WASAPI / AAudio 回调拿到 `output` 后直接调用 `ClientRuntime::pull_playback()`，最终落到 `JitterBuffer::pull()`。中间没有
额外 ring buffer，也没有第二个水位。
