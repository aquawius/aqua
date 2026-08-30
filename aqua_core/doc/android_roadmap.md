# Android Roadmap

## 1. 前提结论

旧 `Android.zip` 不是需要推倒的失败实现。它已经验证过 Android application layer 可以工作，包含
Kotlin/Compose、Service、native bridge、轮询 diagnostics、设置持久化等完整应用骨架。

失败根因属于当时的 Core：组件多、JitterBuffer/缓存模型未冻结、Android API 与旧 Core 强耦合。

当前新版 Core 已经把最关键的边界收紧，因此本次原则是：

> **保留旧 Android 的应用层经验，替换其旧 Core 适配面，不把旧 Core 设计迁回来。**

## 2. 产品约束

第一阶段明确：

- Android 只做 playback；
- API = AAudio；
- Kotlin + Jetpack Compose；
- Exclusive 不实现；
- Client 格式完全服从 Server；
- Android backend 不支持 Server `AudioFormat` 时，直接拒绝 playback start；
- 不加第二个 playback RingBuffer；
- spdlog 直接使用 Android sink 输出 logcat；
- 设备切换不是第一阶段 runtime 功能，默认输出设备优先。

## 3. 目标分层

```text
Jetpack Compose
   │
Kotlin Controller / Service
   │
JNI
   │
C ABI compatibility layer
   │
ClientRuntime
   ├─ gRPC
   ├─ UDP
   ├─ JitterBuffer
   └─ AudioPlayback
          │
        AAudio
```

## 4. 为什么需要 C API

JNI 不应该直接持有 C++ class、`std::string`、`std::span`、`std::expected` 或 C++ callback。C API 提供稳定 opaque handle：

```c
struct aqua_client;
typedef struct aqua_client aqua_client_t;
```

推荐第一版 C API：

```text
aqua_client_create
aqua_client_start
aqua_client_stop
aqua_client_destroy
aqua_client_get_state
aqua_client_get_last_error
aqua_client_get_audio_format
aqua_client_get_diagnostics
```

这些 API 应只做薄 wrapper，业务仍由 `ClientRuntime` 实现。C API 不是第二个 runtime。

## 5. AAudio backend 设计

### 5.1 接到现有抽象

实现：

```text
audio::AudioPlayback
      ↓
AAudioAudioPlayback
```

callback 直接将 AAudio 提供的 `audioData + numFrames` 转成现有：

```text
span<byte> output
    ↓
AudioPlaybackCallback
    ↓
ClientRuntime::pull_playback
    ↓
JitterBuffer::pull
```

因此 Android 不需要自己实现一套 JitterBuffer consumer。

### 5.2 配置策略

第一阶段请求：

```text
performance = LOW_LATENCY
sharing = SHARED
```

不请求 Exclusive。

sample rate / channel count / format 按 Server ConnectResponse 的 AudioFormat 请求；open 后必须重新读取 AAudio 实际
stream configuration 并验证。如果实际格式与 Server 契约不一致，backend 不应偷偷转换，应返回 `FormatUnsupported`/
`BackendFailed`。

### 5.3 PCM 范围

AAudio backend 第一阶段原生支持矩阵按其实际 native PCM 能力实现，至少目标为：

```text
S16LE
S24 packed
S32
F32
```

`U8` 不强制转换；如果设备/backend 不支持则明确拒绝。

### 5.4 Callback 约束

AAudio data callback 与当前 WASAPI callback 共享同一 realtime contract：

- 不加锁；
- 不分配；
- 不做 I/O；
- 不调用 stop/close；
- 必须填满 output；
- 返回有效写入 frame count。

AAudio error callback 不直接 close/stop stream；它只发布 pending error，由非 realtime/control 路径处理。这样保持与
WASAPI“event thread 处理运行期错误”的思想一致。

## 6. Android DeviceManager 第一阶段

先做最小实现：

```text
default OUTPUT device
resolve(nullopt) -> system/default output
```

不先做复杂 AudioDevice enumeration / Bluetooth routing / USB selection。原因是这些都不属于把 Core 跑起来的必要条件，而且会把
Android framework state 引入 Core。

## 7. spdlog

当前 logger 已有 `__ANDROID__` 分支，使用：

```text
spdlog::sinks::android_sink_mt
TAG = aqua
```

因此 Android application 不应再把 native stdout 重定向或重复封装一套 logger。Kotlin 只负责 UI 侧日志（若需要），Core 继续直接
logcat。

## 8. JNI 设计

旧项目采用轮询模型，没有 native -> Kotlin callback。这个经验可以继续保留：

```text
Compose/Controller
    │ 250ms poll
    ├─ state
    ├─ last error
    ├─ diagnostics
    └─ audio format
```

这样可以避免第一阶段 JNI callback 生命周期、JavaVM attach、thread affinity 等额外复杂度。

但旧项目中的配置字段：

```text
jitterBufferMs
jitterDetectWindowPackets
playbackBufferSize
```

不应原样迁移，因为它们属于旧 Core 模型。新 Android UI 应暴露当前 Core 真正支持的 `jitter_buffer_slots` 等参数；如果 UI
第一版不需要高级参数，甚至可以不暴露。

## 9. Gradle / native build

当前根 CMake 已经预置：

```text
android-arm64-debug
android-arm64-release
```

其目标环境：

```text
ABI      = arm64-v8a
Platform = android-28
STL      = c++_shared
Generator= Ninja
Triplet  = arm64-android
```

第一阶段先让 native library 独立成功构建，再接 Android Studio/Gradle packaging。不要把“CMake build failure”和“Compose/JNI
bug”混到一次调试循环中。

## 10. 实施里程碑

### A0：冻结 Core Android contract

交付：

- C API header
- C API error/state enum
- Android-specific backend extension points
- format support matrix
- lifecycle contract

验收：Windows CLI 不回归，C API 不包含 C++ STL 类型。

### A1：AAudio backend

交付：

- `AAudioAudioPlayback`
- Android `AudioDeviceManager`
- factory 选择
- AAudio callback/error handling
- spdlog/logcat

验收：一个最小 native test program 可以连接 mock/真实 ClientRuntime，并让 callback 连续获得 PCM/silence。

### A2：Android native build

交付：

```text
libaqua.so
```

验收：arm64-v8a Debug/Release 构建，依赖打包正确。

### A3：JNI bridge

交付：

- opaque handle 生命周期
- start/stop/state/error/diagnostics/format
- exception/invalid-handle 防护

验收：Android app 不崩溃，native handle 生命周期严格一进一出。

### A4：Compose application

以旧 Android 工程为参考，保留已经验证过的：

- MainActivity
- AquaController
- Foreground Service
- 设置持久化
- diagnostics 页面
- Compose navigation/theme

但把旧 Core 参数和 diagnostics array 全部替换成当前 Core 的接口。

### A5：真机验证

按顺序验证：

```text
Wi-Fi 同网
→ gRPC Connect
→ UDP HELLO/ACK
→ pre-roll
→ PCM playback
→ packet loss / reordering
→ screen off / foreground service
→ audio focus / route change
→ AAudio disconnect
```

最后再做性能、功耗和长时间运行。

## 11. 暂不做

第一阶段明确不进入：

- Android capture / loopback
- Exclusive
- 软件 resampler
- 双缓冲/三缓冲播放架构
- Bluetooth 特殊路由
- 多设备选择 UI
- NAT traversal
- token authentication

这些都可能有价值，但都不是“把现在稳定 Core 移植到 Android”的前置条件。

## 12. 最重要的验收标准

Android 成功不是“Compose 页面能点播放”，而是下面这条链完全闭合：

```text
Server AudioFormat/F
      ↓
ConnectResponse
      ↓
C API
      ↓
ClientRuntime
      ↓
JitterBuffer
      ↓
AAudio callback
      ↓
Android output
```

中间任何一层都不得偷偷改变 PCM 契约、增加未文档化的 buffer 或绕开 Runtime 生命周期。
