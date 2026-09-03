# 线程模型与生命周期

## 1. Server 线程角色

```text
io_context 线程（CLI main 兼）
  ├─ 诊断 timer（1s）
  ├─ control timer（500ms）——capture 切换事务在此同步执行
  ├─ signal handler
  └─ session reaper（独立 strand）

Capture RT 线程
  └─ AudioCapture block 回调
       -> AudioPacketizer
       -> AudioFrameQueue::push
       -> dispatcher wake hint

Capture event 线程
  └─ 运行期错误投递（AUDCLNT_E_DEVICE_INVALIDATED 等）
       -> on_capture_event（只置标志，不做 stop/start）

Dispatcher worker
  └─ drain queue -> encode -> UDP broadcast

UDP IO / Asio handlers
  └─ 接收 HELLO / 发送 datagram（绑定 transport strand）

gRPC worker 线程
  └─ GrpcServer::run()
```

### 实时边界

Capture 回调只允许有界工作：`memcpy`、原子操作、SPSC 入队、一次唤醒提示。禁止 mutex、堆分配、Asio post、网络 I/O 与 gRPC。
设备切换事务（stop → join → start）发生在 control 线程，不进入 RT 路径。

**已知偏差（待修）**：`AudioNetworkDispatcher::publish_from_realtime()` 中的 `std::atomic::notify_one()` 在部分标准库实现
上不是 lock-free（libstdc++ 走全局 futex 表加互斥锁，MSVC 走系统调用），它由 capture RT 线程每帧调用；WASAPI 采集循环在
DATA_DISCONTINUITY 分支也会打日志（spdlog sink 带锁）。二者都在故障/切歌时成簇触发，是 xrun 的潜在来源。

## 2. Client 线程角色

```text
io_context 线程（CLI main 兼；C API 场景为内部 IO 线程）
  ├─ UDP 接收
  ├─ HELLO timer（1s）
  ├─ 诊断 timer（1s）
  └─ control timer（500ms）——playback 恢复与默认设备跟随在此执行

Playback RT 线程
  └─ backend pull 回调 -> JitterBuffer::pull

backend event 线程
  └─ 运行期错误投递 -> on_playback_event（只置标志 + post）
```

gRPC Connect / Disconnect 是同步控制操作，由调用 `start()` / `stop()` 的线程执行。

Android 侧另有：`aqua-lifecycle` 单线程 executor（所有 native 调用串行入队）与主线程（UI、`AudioManager` 设备回调）。

## 3. 音频回调生命周期

capture 与 playback 后端遵守同一契约：

```text
start() 返回成功
    ↓
回调可以开始
    ↓
stop()
    ↓
等待回调不可能再运行（join 音频线程）
    ↓
返回
```

因此 Runtime 的 teardown 可以安全销毁或重建音频组件；后端不允许让回调在 `stop()` 返回后继续访问回调对象或缓冲区。

**禁止在 block / event 回调内调用 `stop()` 或 `start()`**——`stop()` 会 join 该线程导致自死锁。运行期错误必须走 event 回调
置标志，由控制线程执行事务。

## 4. 切换事务的线程交接

两侧切换事务结构相同，都采用 break-before-make：

```text
控制线程：管理状态 = Switching
  捕获 previous_active_device
  backend->stop()          ← 同步 join，返回后旧回调不再访问共享缓冲
  依次尝试候选 [target, previous, system_default]
  首个成功 -> 管理状态 = Running；链耗尽 -> Fatal
```

join 保证了生产者/消费者的唯一性交接：server 侧 packetizer 在事务期间没有生产者，client 侧 JitterBuffer 在事务期间没有
消费者。`AudioFrameQueue` 与 `JitterBuffer` 的既有内容一律保留（结转），不因切换清空。

事务期间 control timer 所在的 io_context 线程会被阻塞（设备打开可能耗时数百毫秒），因此 UDP 接收与诊断输出会短暂停顿——
这是有意的取舍：切换是低频事件，正确性优先于该线程的响应性。

## 5. CallbackGate

Client Runtime 的 UDP 回调与异步事件可能晚于控制操作排队。`CallbackGate` 用 mutex 保护 owner 指针：

- stop / 析构前调用 `detach()`；
- 晚到的回调拿到锁后发现 owner 为空，静默丢弃；
- 回调内用户代码抛出的异常被捕获，不越过异步线程边界。

它解决的是"异步通知访问 runtime 生命周期"问题，不是音频 RT 同步原语。JitterBuffer 的 push / pull 不依赖这个 gate。

## 6. Server reaper

session reaper 使用独立 `ReapState`：

```text
asio::strand
    └─ steady_timer
```

首次启动、周期重挂与取消都在同一 strand 上完成。timer handler 通过 `weak_from_this()` 取回 `ServerRuntime`，因此 reaper
不会延长 runtime 生命周期。

## 7. stop 顺序

### Client

```text
enter Stopping
  ↓
playback.stop()            先切断消费者
  ↓
udp.stop()
  ↓
gRPC Disconnect（best effort）
  ↓
Stopped
```

先停 playback 很关键：它先切断 `pull()` → JitterBuffer 的消费者，避免 teardown 与音频回调交叠。

### Server

```text
enter Stopping
  ↓
capture.stop()             先切断生产者
  ↓
cancel reaper timer
  ↓
dispatcher.stop() + join
  ↓
udp.stop()
  ↓
grpc.shutdown()
  ↓
grpc 线程 join
  ↓
sessions.clear()
  ↓
Stopped
```

## 8. Runtime 状态

- `Created`：尚未启动。
- `Starting`：正在建立设备、控制面与数据面。
- `Running`：正常运行。
- `Degraded`：运行期出现无法自愈的终止条件，需要上层停止。一次性终态。
- `Stopping`：正在 teardown。
- `Stopped`：生命周期结束，不支持再次 `start()`。

Runtime 本身是一次性对象：要更换**音频格式**必须创建新实例。但**设备**可以在会话内切换——capture 与 playback 各自有管理
状态（`CaptureSwitchState` / `PlaybackState`）跟踪切换事务，切换不改变会话、格式与时间线。
