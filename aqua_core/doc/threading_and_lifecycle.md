# 线程模型与生命周期

## 1. Server 线程角色

典型运行时存在：

```text
CLI/main thread
  └─ asio::io_context：诊断/控制 timer、signal

Capture RT thread
  └─ AudioCapture callback
       -> Packetizer
       -> AudioFrameQueue::push
       -> dispatcher wake hint

Dispatcher worker
  └─ queue consume -> encode -> UDP broadcast

UDP IO / Asio handlers
  └─ receive HELLO / send datagrams

gRPC worker thread
  └─ GrpcServer::run()

Reaper strand
  └─ Session timeout timer
```

### 服务器实时边界

Capture callback 不进行：

- mutex
- heap allocation
- Asio post
- network I/O
- gRPC

它只做 bounded memcpy、packetizer、SPSC push 和一次轻量 wake hint。

## 2. Client 线程角色

```text
CLI/main + asio::io_context
  └─ UDP receive / HELLO timer / diagnostics

UDP handler
  └─ decode -> JitterBuffer::push

Playback RT thread
  └─ backend callback -> JitterBuffer::pull
```

gRPC Connect/Disconnect 是同步控制操作，由 `ClientRuntime::start/stop` 的调用线程执行。

## 3. AudioPlayback callback 生命周期

backend 必须保证：

```text
start() returns success
    ↓
callback may begin
    ↓
stop()
    ↓
wait until callback can no longer run
    ↓
return
```

因此 `ClientRuntime::stop_locked()` 可以安全销毁/复用 playback 相关对象；backend 不允许让 callback 在 stop 返回后继续访问 callback 对象。

## 4. CallbackGate

Client Runtime 的 UDP callback / 异步事件可能晚于控制操作排队。`CallbackGate` 用 mutex 保护一个 owner 指针：

- stop/destruction 前可以 `detach()`；
- callback 如果晚到，拿锁后发现 owner=null 即静默丢弃；
- callback 内如果用户通知 callback 抛异常，会被捕获，不越过异步线程边界。

它解决的是**异步通知访问 runtime 生命周期**问题，不是音频 RT 同步原语。JitterBuffer push/pull 自己不依赖这个 gate。

## 5. Server reaper

Server 的 session reaper 使用独立 `ReapState`：

```text
asio::strand
    └─ steady_timer
```

首次启动、周期重挂和 cancel 都在同一 strand 上完成。timer handler 通过 `weak_from_this()` 取回 ServerRuntime，因此 reaper 不会强行延长 runtime 生命周期。

## 6. stop 顺序

### Client

```text
enter Stopping
  ↓
playback.stop()
  ↓
udp.stop()
  ↓
gRPC Disconnect (best effort)
  ↓
Stopped
```

先停 playback 很关键：它先切断 `pull()` → JitterBuffer 的 consumer，避免 runtime teardown 与音频 callback 交叠。

### Server

```text
enter Stopping
  ↓
capture.stop()
  ↓
cancel reaper timer
  ↓
dispatcher.stop() + join
  ↓
udp.stop()
  ↓
grpc.shutdown()
  ↓
grpc thread join
  ↓
sessions.clear()
  ↓
Stopped
```

先停 capture 保证不再产生新 AudioFrame；再停 dispatcher，确保 handoff worker 已经结束；最后关闭网络控制面。

## 7. Runtime 状态

- `Created`：尚未启动。
- `Starting`：正在建立设备、控制面、数据面和 playback/capture。
- `Running`：正常运行。
- `Degraded`：运行期 backend/network terminal condition 已被观察，需要上层停止。
- `Stopping`：正在 teardown。
- `Stopped`：生命周期结束，不支持再次 start。

代码注释和 API 都以“一次性 runtime”为契约；要更换设备或核心格式，创建新的 runtime 实例。
