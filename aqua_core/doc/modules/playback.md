# 模块：AudioPlayback / PlaybackManager / WASAPI / AAudio Playback

## 抽象

`AudioPlayback` 是输出 pull 接口：

```cpp
start(config, pull_callback, event_callback)
stop()
is_running()
stream_info()
generation()          // 纯虚：当前流的代号，无流时 kNoStreamGeneration
```

pull 回调签名：

```text
span<byte> output -> 返回实际写入的 frames
```

backend 必须把未写入区域按 **会话编码的静音字节**补齐（不是一律清零：`PCM_U8` 是 `0x80`），统一经
`AudioFormat::silence_byte()` 取规则、由 `audio/public/audio_fill_silence.h` 的 `fill_silence_tail()` 完成 （WASAPI 与
AAudio 共用），并保证 `stop()` 返回后不再调用回调。与 `AudioCapture` 一样，控制 API 只能由控制线程调用， 禁止在回调内调用
`start()` / `stop()`。

## PlaybackManager

`PlaybackManager`（`src/audio/playback/playback_manager.cpp`）托管回放流生命周期与切换事务，对称 server 侧的
`CaptureManager`：

```text
ClientRuntime --> PlaybackManager --> AudioPlayback --> WASAPI / AAudio
```

- **路由（两根正交的轴）**：`AudioRoute`（给 backend 的**请求**：`System` 交平台路由 / `Application` 绑定到具体端点）+
  `RoutePolicy`（manager 的**决策状态**：意图归属 `None` / `Application` / `User`，加丢失动作）。client 侧丢失动作恒为
  `FallbackToSystem`（`PlaybackManager::kLossAction`——移动端永不主动静音），与 server 采集侧的 `Fatal` 相反。
- **候选链**：显式目标走完整链 `[目标设备, 先前的实际设备, 系统默认]` 去重； nullopt 目标（自动/用户跟随）单候选直达当前默认。成功后给出
  `Switched` /
  `RolledBack` / `FellBackToSystem`；链耗尽 = `Fatal`。
- **意图归属按请求推导**：用户显式选设备即 `User`（sticky；fallback 降级不改变， pin 与自动切回保留）；选 nullopt 即
  `None`；"自动切换播放设备"关的连接在首流成功、读到实际落点后升级为 `Application`（钉住落点，**非** sticky、设备回归**不**自动切回）。
- **防抖（两套独立预算）**：错误驱动 restart 与内部自动跟随（tick/快照/自动切回）共享 10s/3 的**设备丢失**预算；
  而"流刚起来就死"（backend 已 `!is_running()` 且流龄 < 400ms）判为**路由未稳定**，走独立的 5s/8 预算并按 200ms
  节流，**绝不占用**前者。节流命中时不改任何状态直接返回，由 500ms supervision tick 再驱动。用户显式选择不计数并重置**两套**窗口。
- **事件归属**：backend 事件带 stream generation，manager 只放行等于 `active_generation_` 的事件，其余计入
  `stale_events_dropped()` 后丢弃；每笔事务开头**先退认**（置 `kNoStreamGeneration`），因此 teardown 期间旧流的临终错误在
  manager 内即被拦下，不上到 runtime。
- **驱动入口**：`restart()`（同设备重建）、`set_playback_device(target)`（显式选择）、`restart_on_error()`（错误驱动，
  settle 耗尽时降级 `nullopt` 并借设备丢失额度，策略不动）、`adopt_user_intent(id)`（起步回退后补装 sticky，
  只写策略不动流）、`tick()`（两步轮询：① `DeviceSetPoller` 去抖后的设备集合变化 → `on_devices_changed`，**所有归属**；
  ② `None` 归属下比较系统默认设备。返回是否执行了切换事务）、`on_devices_changed(ids)`（设备集合快照，
  Android 由 JNI 推送全集、Windows 由 `tick()` 第 1 步轮询喂入——同一份决策逻辑；以 `switch_seq` 是否变化报告，
  节流空操作报 `false`，`Fatal` 报消费）。

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

启动阶段还有一次 **设备兜底**：若带 `--playback-device-id` 的首次 `start()` 失败，会以系统默认设备重试一次并记日志，避免单个设备不可
用直接导致连接失败。兜底成功后补装原 pin 的 sticky 意图（`adopt_user_intent`），`route_mode` 仍报 `User`、
`requested_device_id` 仍是原选择，设备回归可自动切回——与运行期 fallback"意图不动"一致。

## WASAPI 当前模型

`WasapiAudioPlayback` 自己拥有 audio thread 与 event thread：audio thread 负责实时提交与
`GetCurrentPadding` / `GetBuffer` / `ReleaseBuffer`；event thread 负责运行期错误事件。Runtime 不直接触碰 WASAPI COM 对象。

## AAudio 当前模型（Android）

`AAudioAudioPlayback`（`src/audio/playback/aaudio/`）遵守同一 pull 抽象与 RT 契约：

```text
performance   NONE / LOW_LATENCY（由 Android「低延迟模式」设置选择）
sharing       SHARED（不做 Exclusive）
data callback (audioData, numFrames) -> span<byte> -> ClientRuntime::pull_playback
              上下文从**自己的槽位**读，不读共享成员（在途回调拿不到新流的几何）
error callback 只发布 pending_error_，不 close / stop；先判 slot.generation == live_generation_，
再用 `fatal_reported_.exchange(true)` 抢派发权并经 `dispatch_event()`（`shared_ptr` 拷引用锁外调）派发
stop          只由控制线程执行：第一件事清 live_generation_，再 requestStop + close（close 等待在途 **data**
回调返回；**error** 回调不与 close 同步，靠 `shared_ptr` 活到在途派发结束）；失败清理先关流再转 `retired`，
`requestStart` 成功后复查死流（见 `../aaudio_backend_design.md` 第5节）
user_data     per-stream StreamSlot{self, generation, context}，**不是 this**
              （stream 指针在 close 后可能被新流复用 = ABA，不能当身份令牌）
```

格式策略（决议见 `../aaudio_backend_design.md`）：encoding 与 channels 必须与 server 契约一致；采样率允许系统重采样（回读 实际
stream 配置校验通道/编码）；`framesPerCallback = 0` 自适应设备 burst。回调上下文由所属 `StreamSlot` 经 `shared_ptr`
保活，close 与在途回调竞争时对象不失效；backend 另保留**恰好一个** `retired_slot_`，覆盖"AAudio 的 error callback 不与 close
同步"那一次迟到投递（详见 `../aaudio_backend_design.md` 第5节 第 4 点）。

## 不支持 Exclusive

Aqua 的 playback 产品契约不使用 Windows Exclusive。不要为降低几毫秒在 backend 中引入第二套共享模式/时钟模型——那会改变
现有的回调与生命周期假设。

## 与 JitterBuffer 的关系

WASAPI / AAudio 回调拿到 `output` 后直接调用 `ClientRuntime::pull_playback()`，最终落到 `JitterBuffer::pull()`。中间没有
额外 ring buffer，也没有第二个水位。
