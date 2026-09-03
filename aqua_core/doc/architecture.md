# Core 架构

## 1. 总体结构

Aqua 是一个局域网音频转发系统：server 采集本机音频输出（loopback）或输入设备，切成定长帧后 UDP 广播；client 接收进
JitterBuffer，再按音频时钟播放。

```text
Application layer
  ├─ aqua_server_cli   （Windows 控制台）
  ├─ aqua_client_cli   （Windows 控制台）
  └─ Android App       （Kotlin/Compose，经 C API + JNI 驱动 ClientRuntime）
          │
          ▼
Core runtime
  ├─ ServerRuntime   ──► CaptureManager ──► AudioCapture
  └─ ClientRuntime   ──► PlaybackManager ─► AudioPlayback
          │
          ├─ control plane: gRPC（Connect / Disconnect）
          ├─ data plane:    UDP（HELLO 握手 + Audio 数据报）
          └─ audio core:    Packetizer / AudioFrameQueue / Dispatcher / JitterBuffer
```

### 静态目标

| 目标                | 内容                                                                                       | 说明                                                        |
|---------------------|--------------------------------------------------------------------------------------------|-------------------------------------------------------------|
| `aqua_proto`        | `aqua_service.proto` 生成代码 + `audio_format_converter`                                    | 单独成目标，避免 `aqua_core_base` 依赖 protobuf              |
| `aqua_core_base`    | logger / diagnostics / `SessionManager` / address utils / `UdpTransport` / `NetworkFrame` / 设备管理器 | 两端共享；平台设备后端（WASAPI / AAudio）按平台条件编入        |
| `aqua_server_core`  | `GrpcServer` / `UdpServer` / `AudioCapture` / `CaptureManager` / `AudioPacketizer` / `ServerRuntime` | 不含任何 playback 代码                                      |
| `aqua_client_core`  | `GrpcClient` / `UdpClient` / `JitterBuffer` / `AudioPlayback` / `PlaybackManager` / `ClientRuntime` | 不含任何 capture 代码                                       |
| `aqua_capi`         | C API + Android JNI 桥；产物为共享库 `aqua`                                                  | 默认 `OFF`，Android preset 打开                              |

Server 与 Client 分开编译，是为了让平台后端依赖（采集 / 回放）不传播到另一端。WASAPI 后端仅 Windows 编入，AAudio 后端仅
Android 编入；Linux / macOS 可配置通过，但没有音频后端。

## 2. 端到端数据流

### Server

```text
OS endpoint（loopback = render 混音，input = capture）
       │
       ▼
AudioCapture（CaptureManager 持有，故障时可重建端点）
       │ AudioBlock：变长 PCM 视图，仅回调内有效
       ▼
AudioPacketizer  重切为定长 F 帧
       │ AudioFrame：F × frame_bytes
       ▼
AudioFrameQueue  SPSC 交接（RT → worker）
       │
       ▼
AudioNetworkDispatcher   每帧 encode 一次
       │
       ▼
UdpServer::broadcast     一份共享 datagram 发往所有 Connected endpoint
```

### Client

```text
UDP 接收（transport strand）
       │ NetworkFrame 解码 + payload 长度校验 + 源端点校验
       ▼
JitterBuffer::push       按 sequence 入槽，天然重排
       │
       ▼
JitterBuffer::pull（回放 RT 回调）
       │ 真实 PCM / 补静音 / 跳槽
       ▼
AudioPlayback（PlaybackManager 持有，故障时可重建流）
       │
       ▼
OS output device
```

Android 在 `ClientRuntime` 之外还有两层薄封装：C API（`aqua_capi`，内部自带 io_context 与 500ms 监督 tick）和 JNI 桥
（`aqua_jni`）。它们不引入第二个 runtime，业务规则仍在 `ClientRuntime`。

### 控制面

控制面独立于音频数据面，只负责建/删 session 与下发参数：

```text
Client ── gRPC Connect ─────────► Server   建 Session（Created）
Client ◄─ session_id / UDP endpoint / AudioFormat / F ── Server
Client ── UDP HELLO ────────────► Server   记 NAT endpoint → Connected
Client ◄─ UDP HELLO_ACK ───────── Server
Client ◄══════ UDP Audio datagrams ══════ Server
Client ── gRPC Disconnect ──────► Server（best effort）
```

## 3. 边界原则

### Audio / Network

`AudioFrame` 属于 audio domain，`NetworkFrame` 属于 net domain。UDP 层不知道 PCM 格式、声道数或 F，只做 wire 编解码与
datagram 传输。

### Runtime / Backend

Runtime 决定"何时启动、用什么格式、数据送往哪里"；backend 决定"如何调用 OS audio API"。backend 不持有 runtime 生命周期、
不直接操作 JitterBuffer，也不提供 `switch_device` 之类的策略 API——设备切换由 `CaptureManager` / `PlaybackManager` 编排为
stop → start 序列。

### RT / 非 RT

实时线程只做有界工作：内存拷贝、原子操作、队列操作与同步回调。网络 I/O、gRPC、堆分配、阻塞等待都必须离开音频回调。
设备切换事务（含 stop/join/start）在控制线程执行，不进入 RT 路径。

## 4. 单一权威

- Server 一次运行期间 `AudioFormat` 固定。
- Server 一次运行期间 `frame_count = F` 固定。
- Client 从 `ConnectResponse` 取得这两项，不自行推断。
- JitterBuffer 的 slot 大小 = `F × format.frame_bytes()`。
- 一个 UDP Audio datagram 的 payload 恰好等于一个完整 AudioFrame。

设备切换不改变以上任何一条：`Format immutable` 是切换事务的前提，候选设备不原生支持会话格式即视为该候选失败。

## 5. 生命周期与设备切换

Runtime 是一次性生命周期对象：

```text
Created -> Starting -> Running
                    └-> Degraded
Running/Degraded -> Stopping -> Stopped
```

`stop()` 幂等，生命周期操作由 `lifecycle_mutex_` 串行化。状态含义见 `threading_and_lifecycle.md` §7。

设备切换（capture 与 playback 各自）遵循同一组不变式：

```text
Session alive        restart 不触碰 gRPC / UDP / session
Format immutable     restart 后流格式必须与会话格式一致，无转码、无重协商
Endpoint replaceable capture / playback 流生命周期独立于会话生命周期
Timeline continuous  切换允许 packet gap，但禁止 seq 重置、时间轴重置、会话重建
```

因此：

- **Server**：capture 设备故障按候选链 `[目标设备, 先前的实际设备, 系统默认]` 重建采集端点；链耗尽才停止会话（见
  `capture_switching_design.md`）。
- **Client**：playback 设备故障同样走候选链；间隙由 JitterBuffer 的水位机制吸收（见 `playback_switching_design.md`、
  `buffer_design.md`）。

两侧的唯一权威差异在于韧性承担者不同：client 切换由本侧 JitterBuffer 吸收，server 切换由**对岸** client 的 JitterBuffer
饥饿路径吸收。Server 不为切换新增任何缓冲机制。

## 6. 为什么没有 playback RingBuffer

Client 只保留 JitterBuffer 一层。再加一个独立 RingBuffer 会产生两个水位、两个消费时钟和两个"该不该补/丢"的决策点，使漂移
与边界行为更难解释。回放回调直接从 JitterBuffer 取数据。
