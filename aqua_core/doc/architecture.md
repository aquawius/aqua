# Core 架构

## 1. 总体结构

Aqua 分为三层：

```text
Application layer
  ├─ aqua_client CLI
  └─ aqua_server CLI
          │
          ▼
Core runtime
  ├─ ClientRuntime
  └─ ServerRuntime
          │
          ├─ control plane: gRPC
          ├─ data plane: UDP
          └─ audio abstraction
               ├─ capture
               ├─ packetizer / queue / dispatcher
               ├─ JitterBuffer
               └─ playback
```

Core 再拆成三个静态目标：

```text
aqua_core_base
  logger / diagnostics / session / address / UDP transport / device manager

aqua_server_core
  gRPC server / UDP server / capture / packetizer / server runtime

aqua_client_core
  gRPC client / UDP client / JitterBuffer / playback / client runtime
```

Server 和 Client 有意不在同一个 core target 中编译 capture 与 playback。这样平台后端依赖不会无意义地传播到另一端。

## 2. 端到端数据流

### Server

```text
OS output/input endpoint
       │
       ▼
AudioCapture callback (RT thread)
       │ AudioBlock, 变长
       ▼
AudioPacketizer
       │ AudioFrame, 定长 F
       ▼
AudioFrameQueue
       │ SPSC handoff
       ▼
AudioNetworkDispatcher (worker)
       │ encode once
       ▼
UdpServer::broadcast
       │ one shared datagram
       ├─────────────► session #1 endpoint
       ├─────────────► session #2 endpoint
       └─────────────► ...
```

### Client

```text
UDP socket receive handler
       │ decode + validate payload size
       ▼
JitterBuffer::push
       │ sequence indexed
       ▼
JitterBuffer::pull (audio RT callback)
       │ real PCM / silence / slot skip
       ▼
AudioPlayback backend（Windows=WASAPI / Android=AAudio）
       │
       ▼
OS output device
```

Android 端在 playback backend 之上还有 C API（`aqua_capi`，含内部 IO/监督线程）与 JNI 桥两层薄封装，供
Kotlin/Compose App 轮询式查询使用；它们不引入第二个 runtime，业务全部仍是 `ClientRuntime`。

控制面独立于音频数据面：

```text
Client ── gRPC Connect ──► Server
Client ◄─ session_id / UDP endpoint / AudioFormat / F ── Server
Client ── UDP HELLO ──► Server
Client ◄─ UDP HELLO_ACK ── Server
Client ◄──────── Audio datagrams ─────── Server
Client ── gRPC Disconnect ─► Server (best effort)
```

## 3. 边界原则

### Audio / Network

`AudioFrame` 属于 audio domain；`NetworkFrame` 属于 net domain。UDP 层不知道 PCM 的格式、声道或 frame_count；它只负责 wire
编解码和 datagram 传输。

### Runtime / Backend

Runtime 决定“何时启动、使用什么格式、数据送往哪里”。backend 决定“如何调用 OS audio API”。backend 不拥有 runtime 生命周期，也不直接操作
JitterBuffer。

### RT / 非 RT

实时线程只做有界工作：内存拷贝、原子操作、队列操作和同步 callback。网络 I/O、gRPC、动态容器增长、阻塞等待都必须离开音频
callback。

## 4. 设计上的单一权威

- Server 一次运行期间固定 `AudioFormat`。
- Server 一次运行期间固定 `frame_count = F`。
- Client 从 `ConnectResponse` 取得这两个值，不重新猜测。
- JitterBuffer 的 slot 大小由 `F × format.frame_bytes()` 决定。
- UDP Audio datagram 的 payload 必须等于一个完整 AudioFrame。

## 5. 生命周期模型

所有 runtime 都是一生命周期对象：

```text
Created -> Starting -> Running
                    └-> Degraded
Running/Degraded -> Stopping -> Stopped
```

`stop()` 是幂等的；生命周期操作由 `lifecycle_mutex_` 串行化。Client 的 `stop()` 会先停 playback，再停 UDP，再 best-effort
Disconnect。Server 则先停 capture、再停 dispatcher/UDP、再 shutdown gRPC 并 join worker、最后清理 session。

## 6. 为什么没有 playback RingBuffer

当前设计只保留 JitterBuffer 作为 client playback buffer。再加一个独立 RB
会产生两个独立水位、两个消费时钟和两个“该不该补/丢”的控制点，反而使漂移和边界行为更难解释。当前 playback backend callback
直接从 JitterBuffer 取数据。
