# 模块：AudioPlayback / WASAPI / AAudio Playback

## 抽象

`AudioPlayback` 是 output pull interface：

```cpp
start(config, callback, event_callback)
stop()
is_running()
```

callback：

```text
span<byte> output -> return frames_written
```

backend 必须把未写入区域清零，并保证 stop 返回后不再调用 callback。

## Client 接线

ClientRuntime 创建 playback backend 后，把 Server ConnectResponse 的 format 强制写入 `AudioPlaybackConfig.format`：

```text
ConnectResponse.audio_format
        ↓
AudioPlaybackConfig.format
        ↓
backend start
```

如果 backend 不支持，Runtime 启动失败；不会尝试“接近格式”。

## WASAPI 当前模型

WASAPI backend 自己拥有 audio thread / event thread。audio thread 处理 audio
event、GetCurrentPadding、GetBuffer/ReleaseBuffer；event thread 处理运行期 error event。Runtime 不直接摸 WASAPI COM 对象。

## AAudio 当前模型（Android）

`AAudioAudioPlayback`（`src/audio/playback/aaudio/`）遵守同一 pull 抽象与 realtime contract：

```text
performance   LOW_LATENCY
sharing       SHARED（不做 Exclusive）
data callback audioData + numFrames -> span<byte> -> ClientRuntime::pull_playback
error callback 只发布 pending_error_，不 close/stop
stop          只由控制线程执行：requestStop + close（close 等待在途回调返回）
```

格式策略（决议见 `../aaudio_backend_design.md`）：编码与声道必须与 Server 契约一致；
采样率允许系统重采样（回读实际 stream 配校验通道/编码）；framesPerCallback = 0 自适应设备
burst。callback 上下文经 shared_ptr 保活，close 与在途回调竞争时对象不失效。

## 不支持 Exclusive

当前 Aqua playback 产品契约不使用 Windows Exclusive。不要为了“降低几毫秒”在 backend 中加入另一套共享模式/时钟模型；那会改变当前
callback / lifecycle 假设。

## 与 JitterBuffer 的关系

WASAPI callback 得到 `output` 后直接调用 Runtime 的 `pull_playback()`，最终落到 JitterBuffer::pull。这里没有额外 RB，也没有第二个
watermark。
