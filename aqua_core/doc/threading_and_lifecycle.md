# Threading & Lifecycle

## 1. Runtime lifecycle

Runtime 是 one-shot：

```text
Created -> Starting -> Running / Degraded -> Stopping -> Stopped
```

`start()` 与 `stop()` 内部由 `lifecycle_mutex_` 串行化。因此：

- 两个线程同时调用 `start()` 不会重复启动；
- `stop()` 可以与正在执行的 `start()` 并发调用；
- 后调用者会等待 lifecycle critical section；
- `Stopped` 后不能 restart。

不要把 `state_` atomic 本身误认为替代 lifecycle mutex；atomic 用于异步 observer/callback，mutex 负责 startup/teardown 的互斥。

## 2. Server ownership

ServerRuntime 持有：

```text
AudioDeviceManager
AudioCapture
SessionManager(shared_ptr)
UdpServer
AudioPacketizer
AudioFrameQueue
AudioNetworkDispatcher
gRPC server + worker thread
reap timer state
```

正常停止顺序：

```text
capture stop
-> dispatcher stop
-> UDP stop
-> gRPC shutdown
-> gRPC thread join
-> session/reaper teardown
-> Stopped
```

所有 callback 在拥有方 teardown 前必须被停止或 detach。

## 3. Client ownership

ClientRuntime 持有：

```text
AudioDeviceManager
AudioPlayback
GrpcClient
UdpClient
JitterBuffer
CallbackGate
```

Playback callback 只能访问 pre-created JitterBuffer 与 atomic diagnostics；不能直接管理 Runtime 生命周期。

## 4. Audio backend callback rules

`AudioCapture` / `AudioPlayback` 的 realtime callback：

- `noexcept`
- no lock
- no allocation
- no blocking
- 不调用 start/stop
- 不销毁 owner

`event_callback` 用于 backend 内部异步故障通知，不是 RT data path。

## 5. UDP transport planes

`UdpTransport` 把 socket data plane 放在 Asio strand；配置 plane 由 `config_mutex_` 保护：

```text
configuration plane
    config_mutex_
    ├─ open/bind
    ├─ set_remote
    ├─ stop
    └─ local_endpoint

async data plane
    strand
    ├─ receive
    ├─ send
    └─ socket handler
```

不要在配置锁中等待 strand；不要把 RT audio path 带入配置锁。

## 6. Dispatcher

```text
capture RT
  -> AudioFrameQueue.push()
  -> generation increment / optional notify
  -> dispatcher worker
  -> NetworkFrame encode
  -> UdpServer.broadcast()
```

队列 wake-up 使用 generation 防止 `load -> wait` 丢唤醒；spurious wakeup 不影响 correctness。


## 7. UDP configuration/data plane split

`UdpTransport` 的配置操作（open/bind/set_remote/local endpoint snapshot）由 `config_mutex_` 保护；异步 receive/send handler 与 socket 生命周期仍归 transport strand。固定 UDP listener 不启用 `SO_REUSEADDR`。
