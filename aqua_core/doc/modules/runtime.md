# 模块：Runtime

`ServerRuntime` 与 `ClientRuntime` 是两端的装配与生命周期边界：它们持有网络、会话、队列与音频组件，并把音频组件交给各自的
切换管理器（`CaptureManager` / `PlaybackManager`）托管。本文档只描述编排与职责，切换事务细节见
`capture_switching_design.md` / `playback_switching_design.md`，线程归属见 `threading_and_lifecycle.md`。

## 1. ServerRuntime

### 构造期

构造期只做**不会失败**的解析与对象创建，失败一律留给 `start()` 报出：

```text
resolve capture device（按 source 方向）  -> effective_capture_device_     仅用于探测格式
resolve AudioFormat（用户指定 ?: 设备 mix format） -> effective_format_
resolve F（auto ?: 显式，受 MTU 预算约束）        -> effective_frame_count_
构造 SessionManager / UdpServer / AudioPacketizer / AudioFrameQueue / AudioNetworkDispatcher
```

`effective_capture_device_` 只服务于格式探测——packetizer 与队列的几何必须在 `start()` 之前确定。**运行期不走这个字段**：
采集路由来自 `config.capture.device`（为空 = 跟随系统，有值 = 指定设备），切换时由 `CaptureManager` 重新解析候选设备。

### start() 顺序

```text
Created -> Starting
  校验 format / F / queue slots / timeout / 端口 / 几何 / IP 合法性
  创建 ReapState（reaper strand + timer）
  创建 CaptureManager（内部持有平台 AudioCapture 后端）
  bind UDP + 启动接收循环
  启动 dispatcher worker
  启动 capture（并校验 backend 实际格式 == effective_format_）
  构造 gRPC server
  spawn gRPC worker 线程
Running
  在 strand 上挂 session reaper
```

任意步骤失败都回到 `stop_locked()`，按逆序清理已创建的资源；Runtime 不向调用者暴露"半启动"状态。

### 数据路径

```text
AudioCapture 回调（RT 线程）
  └─ on_capture_block -> packetizer_.push
        └─ frame_queue_.push -> dispatcher_.publish_from_realtime()
              └─ dispatcher worker: drain queue -> encode -> udp_.broadcast
```

### 运行期控制：service_capture_switching()

由 CLI control timer 每 500ms 在 io_context 线程调用（经 `lifecycle_mutex_` 与 `start()` / `stop()` 串行化）。它是 capture
切换的决策表：

```text
capture 管理状态 == Fatal                 -> 返回 Fatal（CLI 据此 stop）
设备错误待处理（DeviceDisconnected）      -> CaptureManager::restart_on_error()
                                             成功后清零 last_audio_error_
否则                                       -> CaptureManager::tick()
                                             （FollowSystem 轮询系统默认设备变化并跟随）
```

restart 事务（stop → join → start）在该调用内同步完成。期间 packetizer 没有生产者，client 侧感知为一次普通网络抖动。
错误驱动与默认跟随共用同一个 10s / 3 次的重试预算，超限即 Fatal。

### 运行期事件：on_capture_event()

backend 事件回调运行在 backend 的 event 线程，只做标志置位，绝不执行 stop/start：

| 事件                  | 处理                                                    |
|-----------------------|-----------------------------------------------------------|
| `DeviceDisconnected`  | 置 `capture_device_error_pending_`，等 control tick 触发切换；**不置 Degraded** |
| 其它（后端内部错误等） | 置 `last_audio_error_`，Runtime 状态迁 `Degraded`（CLI 下一 tick 停止）         |

只用 `DeviceDisconnected` 作为切换触发源。静音、低能量、"长时间无音频"都不能用来推断设备故障——loopback 在没有 render
client 时静默并产出合成静音是合法稳态。

## 2. ClientRuntime

### 启动顺序

```text
Created
  校验 client 配置
  创建 DeviceManager
  创建 PlaybackManager（内部持有平台 AudioPlayback 后端）
  gRPC 连接 + Connect RPC
  校验返回的格式 / F / payload 几何
  创建 JitterBuffer 与回放流水线
  配置 UDP remote
  启动 UDP 接收
  启动 HELLO 保活
  启动 AudioPlayback
Running
```

顺序的意义是：在回放回调开始之前，网络目标、格式、F 与 JitterBuffer 存储都已就绪。

失败时回到 `stop_locked()`，按逆序清理。

### 数据路径

UDP 接收回调在 transport strand 上构造 `AudioFrame{sequence, frame_count, pcm_view}` 并 `jb->push()`；JitterBuffer 复制
数据，不依赖接收缓冲区生命周期。回放回调只做 `JitterBuffer::pull(output)`。

### 运行期控制

CLI control timer 每 500ms 调用两个入口（同一控制线程串行）：

- `service_playback_recovery()`：设备错误标志或"静默死流"（管理状态 Running 但 backend 已停止）→ 走
  `PlaybackManager::restart_on_error()` 候选链；成功后清零错误通道。
- `service_default_device_follow()`：转发到 `PlaybackManager::tick()`，在 FollowSystem 模式下轮询系统默认输出设备变化。

此外 `notify_devices_changed(ids)` 可由任意线程调用（Android 的 Kotlin 回调线程即如此）：事件 post 到 io_context，经 1s
合并窗口去抖后，由 `PlaybackManager::on_devices_changed()` 完成全部路由决策。

## 3. Degraded

`Degraded` 表示"运行期出现了无法自愈的终止条件"，由 CLI control poll（500ms）观察并 stop。它是**一次性终态**，没有回到
`Running` 的路径。

能进入 `Degraded` 的只有两类情况：

1. 非设备的后端/网络终止条件（capture 的非 `DeviceDisconnected` 错误、client 的非设备类错误）；
2. 切换链耗尽或重试预算超限（管理状态 Fatal）。

设备失效本身**不再**把 runtime 打成 `Degraded`——它是可自愈的，由切换事务吸收。
