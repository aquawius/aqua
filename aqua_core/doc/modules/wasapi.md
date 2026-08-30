# 模块：WASAPI 后端

## 1. Playback

`WasapiAudioPlayback` 自己管理：

```text
audio_thread
 event_thread
 stop_event
 audio_event
 error_event
 running flag
```

控制线程调用 start/stop；audio thread 负责实时音频提交，event thread 处理运行期异常。

Runtime 只看到 `AudioPlayback` 抽象。

## 2. Playback callback

WASAPI 后端在需要输出时调用 Core callback。Core callback 最终直接进入：

```text
ClientRuntime::pull_playback()
        ↓
JitterBuffer::pull()
```

没有额外 ring buffer。

## 3. Capture

`WasapiAudioCapture` 以事件驱动方式读取 endpoint buffer。一次事件可产生多个 packet，因此输出给 Core 的 `AudioBlock`
是变长的；Packetizer 再负责固定 F。

## 4. Loopback starvation

loopback 在 quiescence 期间可能不产生 audio event。当前实现用 20ms bounded wait，超时后主动探测；无数据时按墙钟生成 bounded
synthetic silence，维持 server audio timeline。

连续 2 次 timeout 才把诊断状态标为 Starved；synthetic silence 从第一次 timeout 即可产生。

## 5. Device lifetime

Runtime 在启动前解析 device；backend 不负责“自动换设备”。设备变化是运行期错误，应通过 event callback 告知 Runtime。重建
stream 属于更高层的 reconnect/restart policy。

## 6. Exclusive

Aqua 当前产品契约不采用 WASAPI Exclusive。任何试图以独占模式重新设计 buffer/clock 的实现都不属于当前架构。
