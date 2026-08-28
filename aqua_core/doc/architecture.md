# Aqua 当前架构与实现手册

> 状态：**实现基线 / 架构冻结候选（v5 wake protocol）**
>
> 本文不是“未来设计草案”，而是对当前源码实现、线程模型、数据契约、生命周期、协议和测试的统一说明。若本文与旧讨论、旧设计草案冲突，以当前源码和本文为准。
>
> 适用代码：`aqua_core` + `aqua_app`。当前生产后端已实现 Windows/WASAPI；Linux/macOS/Android 仍按接口层设计，具体 backend 尚未全部实现。

---

## 1. 项目目标与总体结构

Aqua 是一个低延迟音频共享系统：Server 采集 PCM，经控制面建立 session 后通过 UDP 数据面传输；Client 接收 UDP 音频，使用单一 JitterBuffer 处理乱序、丢包和播放水位，再交给本地音频回放 backend。

系统分为两个可执行程序：

```text
Server
  WASAPI Capture
      ↓
  AudioBlock
      ↓
  AudioPacketizer
      ↓
  AudioFrameQueue          ← 仅 RT→network handoff
      ↓
  AudioNetworkDispatcher   ← 唯一 AudioFrame → NetworkFrame
      ↓
  UdpServer
      ↓
  UdpTransport
      ↓
  UDP

Client
  UDP
      ↓
  UdpTransport
      ↓
  UdpClient                ← protocol / sender validation
      ↓
  AudioFrame
      ↓
  JitterBuffer              ← client 音频时间轴唯一应用层缓冲
      ↓
  AudioPlayback
      ↓
  WASAPI Playback
```

控制面独立于上述音频数据面：

```text
Client ── gRPC Connect ──→ Server
Client ←─ ConnectResponse ─ Server
Client ── gRPC Disconnect → Server

UDP HELLO / HELLO_ACK 负责 data-plane association + keepalive。
```

核心原则：

1. **Audio domain 与 Network domain 分离。** `net` 层不理解 `AudioFrame`。
2. **Server capture realtime callback 不触碰网络控制面。** 不 encode、不加 SessionManager 锁、不创建 shared_ptr、不提交 Asio task。
3. **Client 只有一个应用层音频缓冲：JitterBuffer。** Server 的 handoff queue 不属于播放缓冲模型。
4. **控制面和数据面分离。** gRPC 管 session/format/endpoint；UDP 管 HELLO 与 Audio datagram。
5. **Runtime 是 composition root。** 它负责创建、连接、停止组件，不把实时 backend loop 上收。

---

## 2. 构建与目标拆分

### 2.1 CMake 目标

`aqua_core/CMakeLists.txt` 将代码拆为：

- `aqua_core_base`：音频基础类型、格式转换、设备管理、地址工具、日志、诊断等跨 server/client 共用能力。
- `aqua_server_core`：Server 侧 Capture、Packetizer、Session、UDP Server、gRPC Server、Network Dispatcher、ServerRuntime。
- `aqua_client_core`：Client 侧 Playback、JitterBuffer、UDP Client、gRPC Client、ClientRuntime。
- 测试目标按 `audio` / `net` / `diagnostics` / `logger` 等子目录组织。

接口头位于：

```text
aqua_core/include/aqua/...
```

backend 实现在：

```text
aqua_core/src/audio/<module>/<backend>/
```

例如 Windows WASAPI：

```text
src/audio/capture/wasapi/wasapi_audio_capture.*
src/audio/playback/wasapi/wasapi_audio_playback.*
src/audio/devices/wasapi/wasapi_device_manager.*
src/audio/wasapi/wasapi_com.h
```

backend 私有头不被公共 domain API 暴露。

### 2.2 CMake preset / 工具链

项目使用现代 CMake preset + vcpkg；Windows 主验证环境是 MSVC + WASAPI + Asio + gRPC/protobuf。最低 CMake 版本由项目当前 `CMakeLists.txt` 固定，不在 runtime 层做平台判断。

---

## 3. Audio Domain 类型体系

### 3.1 `AudioFormat`

文件：

```text
include/aqua/audio/audio_format.h
src/audio/audio_format_converter.cpp
```

`AudioFormat` 描述 PCM 本身：

```text
encoding
channels
sample_rate
```

支持的编码包括 S16LE、S32LE、F32LE、S24LE、U8。格式有上限校验（channel / sample rate），并提供：

- `is_valid()`
- `bytes_per_sample()`
- `frame_bytes()`
- `bytes_for_frames(frame_count)`
- `frames_from_bytes(data_size)`
- `frame_count_for_budget(format, bytes)`

其中：

```text
sample frame = 所有声道在一个采样时刻对应的一组 sample
frame_bytes  = bytes_per_sample × channels
PCM bytes    = frame_count × frame_bytes
```

`AudioFormatConverter` 只负责 domain ↔ protobuf 的值转换，不承担网络 packet size、latency 或设备能力。

### 3.2 `AudioBlock`

文件：

```text
include/aqua/audio/audio_block.h
```

定义为非拥有 PCM span：

```text
AudioBlock = { data }
```

特点：

- capture backend 的原始输出单位；
- 长度不固定；
- 不携带 sequence；
- `data` 仅在 capture callback 当前调用期间有效。

### 3.3 `AudioFrame`

文件：

```text
include/aqua/audio/audio_frame.h
```

定义：

```text
sequence
frame_count
data
```

约束：

```text
frame_count = session 固定 F
data.size() = F × format.frame_bytes()
```

`AudioFrame::is_well_formed()` 用于 domain 层结构校验。

`sequence` 在 server 侧由 `AudioPacketizer` 唯一产生，进入网络后保持原值，不重新编号。

### 3.4 `NetworkFrame`

文件：

```text
include/aqua/net/udp/network_frame.h
src/net/udp/network_frame.cpp
```

这是 UDP wire representation，不是 Audio domain 类型。

当前布局：

```text
Audio:
[0]      type        1 byte
[1..8]   sequence    u64 little-endian
[9..]    PCM payload

Hello / HelloAck:
[0]      type        1 byte
[1..4]   session_id  u32 little-endian
```

Audio payload 必须：

```text
1 <= payload <= UDP_AUDIO_PAYLOAD_BYTES
```

当前安全预算：

```text
UDP_AUDIO_PAYLOAD_BYTES = 1443
```

`frame_count` 不进入 UDP packet；Client 通过 gRPC ConnectResponse 获得 F。

---

## 4. 设备抽象与 backend 工厂

### 4.1 `AudioDeviceId`

文件：

```text
include/aqua/audio/devices/audio_device.h
```

是不透明的字符串身份。代码不解析 ID 内部结构，也不保证跨 backend / 跨机器稳定。

### 4.2 `AudioDevice`

包含：

```text
id
name
direction
is_default
```

设备对象不携带 `AudioFormat`。Format 属于具体 stream。

### 4.3 `AudioDeviceDirection`

```text
INPUT   = capture source
OUTPUT  = output endpoint / loopback source
NONE    = sentinel
```

loopback 不是独立设备类型，而是：

```text
capture direction == OUTPUT
```

### 4.4 `AudioDeviceManager`

文件：

```text
include/aqua/audio/devices/audio_device_manager.h
src/audio/devices/audio_device_manager_factory.cpp
src/audio/devices/wasapi/wasapi_device_manager.*
```

主要职责：

- `enumerate(direction)`：列设备；
- `default_device(direction)`：获取默认设备；
- `resolve(direction, optional<id>)`：启动时将 default / explicit device 解析为具体 `AudioDevice`。

`resolve()` 同时校验 requested device 与 direction 是否匹配。

---

## 5. Capture / Playback 抽象

### 5.1 `AudioCapture`

文件：

```text
include/aqua/audio/capture/audio_capture.h
include/aqua/audio/capture/audio_capture_config.h
src/audio/capture/audio_capture_factory.cpp
```

接口采用 push model：

```text
start(config, block_callback, event_callback)
stop()
```

数据 callback：

```cpp
std::move_only_function<void(const AudioBlock&) noexcept>
```

事件 callback：

```cpp
std::move_only_function<void(AudioError) noexcept>
```

数据 callback 是 realtime path；event callback 用于运行期 backend fault，例如设备断开。

生命周期保证：

- `stop()` 返回后不再产生 callback；
- backend 允许再次 start；
- 不允许从 backend callback 内调用 `start()/stop()`。

### 5.2 `AudioPlayback`

文件：

```text
include/aqua/audio/playback/audio_playback.h
include/aqua/audio/playback/audio_playback_config.h
src/audio/playback/audio_playback_factory.cpp
```

采用 pull model：

```cpp
std::move_only_function<std::uint32_t(std::span<std::byte>) noexcept>
```

callback 返回实际填充 sample frame 数；backend 未填满的 output 由 backend 补静音。

### 5.3 `AudioError`

文件：

```text
include/aqua/audio/audio_error.h
```

用于跨 backend / runtime 的统一错误表示，例如：

```text
None
InvalidArgument
FormatUnsupported
DeviceNotFound
DeviceDisconnected
PermissionDenied
NotSupported
BackendFailed
```

具体枚举以源码为准。

---

## 6. Windows/WASAPI 实现

### 6.1 Device Manager

文件：

```text
src/audio/devices/wasapi/wasapi_device_manager.*
```

负责枚举 Windows audio endpoint、默认设备解析及 backend 内部 device ID 转换。

COM 初始化是线程级状态；DeviceManager 在调用线程建立 COM apartment。

### 6.2 WASAPI Capture

文件：

```text
src/audio/capture/wasapi/wasapi_audio_capture.*
```

当前模型：

```text
WASAPI Shared Mode
+ AUDCLNT_STREAMFLAGS_EVENTCALLBACK
```

loopback 模式额外使用：

```text
AUDCLNT_STREAMFLAGS_LOOPBACK
```

内部有独立 realtime thread；该线程：

1. 初始化自己的 COM apartment；
2. 等待 WASAPI event；
3. 获取 audio packets；
4. 将 PCM 以 `AudioBlock` 形式同步交给应用 callback；
5. 使用 MMCSS `Pro Audio`。

Capture callback 所在 realtime thread 不能做：

```text
mutex
allocation
I/O
Asio
SessionManager
wire encode
```

### 6.3 WASAPI Playback

文件：

```text
src/audio/playback/wasapi/wasapi_audio_playback.*
```

优先路径：

```text
IAudioClient3::InitializeSharedAudioStream
```

通过 `GetSharedModeEnginePeriod` 选择合法低延迟 period；失败时退回 `IAudioClient::Initialize` Shared + Event 路径。

Playback realtime thread：

```text
MMCSS Pro Audio
+ event driven
+ application pull callback
```

共享模式下允许 output callback 的 `K` 与网络 AudioFrame 的 `F` 不同；JB 通过 `read_offset` 处理这种粒度差异。

### 6.4 `wasapi_com.h`

这是 backend 私有 RAII helper：

- 线程级 COM 初始化/反初始化；
- 正确处理 `RPC_E_CHANGED_MODE`；
- 公共 audio API 不暴露 COM。

---

## 7. AudioPacketizer

文件：

```text
include/aqua/audio/packetizer/audio_packetizer.h
src/audio/packetizer/audio_packetizer.cpp
```

职责：把 backend 给出的变长 `AudioBlock` 切成固定 `AudioFrame`。

固定参数：

```text
F = frame_count
B = frame_bytes
slot_size = F × B
```

内部预分配一个 `pending_`。

每次 `push(pcm)`：

```text
copy pcm → pending
pending 满
    ↓
同步调用 sink(AudioFrame)
    ↓
sequence++
    ↓
pending_size = 0
```

RT 契约：

- 构造期分配；
- `push()` 不加锁；
- `push()` 不执行 I/O；
- sink 必须同步返回、不得阻塞/分配/抛异常；
- Frame data 指向内部 `pending_`，只在 sink 调用期间有效。

当前 sequence 从 0 开始，溢出沿 `uint64_t` 自然回绕；当前协议没有额外 epoch。

---

## 8. Server RT→Network handoff：AudioFrameQueue

文件：

```text
include/aqua/audio/queue/audio_frame_queue.h
```

这是一个固定容量 SPSC 队列，**不是播放缓冲**。

所有权：

```text
producer = capture realtime thread
consumer = AudioNetworkDispatcher worker
```

容量默认：

```text
4 slots
```

默认每帧约 3ms 时，handoff backlog 上限约 12ms。

### 8.1 Slot 生命周期

```text
Empty / 可写
    ↓ producer memcpy
Published by head.release
    ↓ consumer acquire
Consumer reads
    ↓ tail.release
Producer may reuse slot
```

producer / consumer 只各自推进自己的 cursor。

### 8.2 满队列策略

```text
head - tail >= capacity
    ↓
drop newest
```

目的：不允许 handoff queue 通过堆积老音频换取越来越大的 latency。

持续拥塞最终在 client 侧体现为 sequence gap，由 JB 按普通丢帧处理。

### 8.3 `PushResult`

当前返回：

```cpp
struct PushResult {
    bool accepted;
    bool should_notify;
};
```

其中 `should_notify` 是一个 wake hint：本次 slot 发布后，如果 consumer cursor 仍等于本次 push 前的 producer cursor，说明该 push 仍可能是第一个待处理项，应尝试唤醒 worker。它不是并发后的 queue-state 真值。

它是**唤醒提示**，不是并发后的 queue-state 真值。

### 8.4 Generation-based wake protocol

完整协议：

Producer：

```text
push 成功
   ↓
wake_generation.fetch_add(1, release)   ← 每帧一次
   ↓
if (should_notify)
    wake_generation.notify_one()         ← 仅 empty-before-push
```

Worker：

```text
drain()
  ↓
queue non-empty? ── yes → continue drain
  ↓ no
observed = generation.load(acquire)
  ↓
再查 queue
  ↓
stopped? / queue non-empty?
  ↓
否则
wait(observed)
```

两个原子操作职责不同：

- `generation.fetch_add()`：关闭 `load(observed) → wait(observed)` 之间的新数据发布竞态；
- `notify_one()`：真正唤醒已经阻塞在 `wait()` 上的 worker。

因此：

```text
notification 不是状态本身；
generation 也不能替代已阻塞线程所需的 notify。
```

最重要的实时保证：**capture callback 每个成功 frame 只做一次 atomic RMW，不执行 unconditional wake primitive。**

---

## 9. AudioNetworkDispatcher

文件：

```text
include/aqua/runtime/audio_network_dispatcher.h
src/runtime/audio_network_dispatcher.cpp
```

它是 server 的唯一 AudioFrame→NetworkFrame 数据面 dispatcher。

线程模型：

```text
独立 std::thread
```

不放到 Asio `io_context` 的理由：

- capture RT 需要唤醒它；
- RT 不允许 `asio::post()`；
- dispatcher 工作包含 wire encode、SessionManager snapshot 和 UDP enqueue，它们不属于 realtime path。

### 9.1 drain

每个 frame：

```text
AudioFrame
  ↓
NetworkFrame::audio()
  ↓
encode()
  ↓
shared_ptr<vector<byte>>
  ↓
UdpServer::broadcast()
```

encode 失败不会让 worker 线程退出；通过 `encode_failures` 统计。

### 9.2 Stop drain

`stop()`：

```text
stop_requested = true
wake worker
join worker
```

worker 退出循环前/退出循环后都会确保最终 `drain()`，因此 stop 前已经进入 handoff queue 的 frame 不被无条件遗留。

### 9.3 Accounting

当前统计分层：

```text
frames_encoded
frames_broadcast
frames_without_clients
encode_failures
dispatch_failures
queue dropped_frames
```

其中：

```text
frames_encoded
    = wire encode 成功的 AudioFrame 数

frames_broadcast
    = encode 成功且至少存在一个 connected session 的 frame 数

frames_without_clients
    = encode 成功但 snapshot 为空的 frame 数

encode_failures
    = encode 路径失败的 frame 数

dispatch_failures
    = broadcast 失败（例如 snapshot/dispatch 例外）的 frame 数
```

UDP socket 的实际 tx 统计由 `UdpTransportStats` 单独负责，不与 application-level accounting 混合。

---

## 10. UdpTransport

文件：

```text
include/aqua/net/udp/udp_transport.h
src/net/udp/udp_transport.cpp
```

这是 server/client 共用的唯一 UDP transport 实现。

特点：

- 不继承 server/client base；
- 自己管理 socket；
- 发送可由任意业务线程调用；
- socket async operation 串行运行于 transport strand；
- 接收 handler 通过 strand 安装。

### 10.1 发送模型

存在两个不同概念：

```text
pending queue
in-flight datagram
```

pending queue 使用 mutex 保护，有界：

```text
UDP_MAX_QUEUED_DATAGRAMS = 64
```

`async_send_to` 开始后，datagram 从 pending queue 移到独立 `in_flight`。

这保证 queue overflow 永远不会删除正在进行 async send 的 packet。

### 10.2 发送泵

只有一个 strand-side send pump：

```text
pending queue non-empty
    ↓
move front → in_flight
    ↓
async_send_to
    ↓ completion
clear in_flight
    ↓
继续泵送
```

多个业务线程连续调用 `send_to_shared()` 最多只需要安排一个 pump task，避免一个 datagram 对应一个无界 executor submission。

### 10.3 Overflow

pending queue 满时：

```text
drop oldest pending datagram
```

注意：这里只 drop pending queue 中尚未开始发送的 packet；in-flight 不受影响。

### 10.4 Socket buffer

UDP transport 会尽量设置：

```text
UDP_RECV_BUFFER_BYTES = 64 KiB
UDP_SEND_BUFFER_BYTES = 64 KiB
```

设置失败是 debug/warn 级降级，不改变核心语义。

### 10.5 地址族

首次 open/bind/set_remote 时确定 IPv4/IPv6 socket family；已经打开的 v4 socket 不允许再切到 v6，反之亦然。

地址只接受 IP literal，不自动做 DNS resolve。

IPv6 host:port 字符串支持 `[addr]` 形式。

---

## 11. UDP Server

文件：

```text
include/aqua/net/udp/udp_server.h
src/net/udp/udp_server.cpp
```

`UdpServer` 是 session-aware protocol layer，不负责音频 domain。

接收流程：

```text
UDP datagram
   ↓
NetworkFrame::decode
   ↓
PacketType::Hello
   ↓
SessionManager::establish_session(session_id, sender)
   ↓
HelloAck
```

因此：

```text
UdpServer knows:
    PacketType
    session_id
    endpoint
    datagram

UdpServer does NOT know:
    AudioFrame
    AudioFormat
    JitterBuffer
```

### 11.1 `broadcast()`

输入：

```cpp
shared_ptr<const vector<byte>> datagram
```

流程：

```text
SessionManager::snapshot_connected()
    ↓
connected_scratch_
    ↓
per-session send_to_shared(endpoint, datagram)
```

`connected_scratch_` 只在 AudioNetworkDispatcher 单一 worker 上使用。该线程归属是不变量；当前 API 不提供跨线程调用保证，
未来若增加第二个 broadcast caller，需要重新处理所有权。

---

## 12. UDP Client

文件：

```text
include/aqua/net/udp/udp_client.h
src/net/udp/udp_client.cpp
```

它负责 client-side UDP protocol，而不是 audio playback。

### 12.1 Audio receive

`start_receive(expected_payload_bytes, FrameHandler)` 接收的是：

```text
sender endpoint
wire bytes
```

先校验：

```text
sender == configured remote endpoint
```

再：

```text
NetworkFrame::decode
→ PacketType::Audio
→ payload size == expected_payload_bytes
→ callback(sequence, pcm span)
```

之后由 ClientRuntime 再把它构造成 `AudioFrame`。

### 12.2 HELLO

`start_hello(session_id, interval, on_liveness_failure)`：立即发送首个 HELLO；之后每个 interval 检查 ACK generation，并发送下一次 HELLO。

```text
strand
  ↓
创建 steady_timer
  ↓
每 interval
  ↓
NetworkFrame::hello(session_id)
  ↓
UdpTransport::send()
```

timer 使用 weak capture，防止 `State → timer handler → State` 自持有环。

当前 HELLO 的意义：

- 维持 server session 活性；
- 刷新 NAT endpoint；
- 建立/维护 data-plane association。

### 12.3 Client HELLO_ACK 活性

Client 不把“没有 AudioFrame”视为断连。`UdpClient` 对正确 session_id 的 HELLO_ACK 维护 ACK generation，并在每个 HELLO interval 检查是否出现新的 ACK。连续 `HELLO_ACK_MISS_THRESHOLD`（默认 3）个 interval 未观察到 ACK 时，只通过 liveness callback 通知 Runtime；`ClientRuntime` 才负责将 `Running/Starting` 锁存为 `Degraded`。单次 ACK 到达会清零连续 miss 计数；ACK age 与 miss count 只用于诊断。

### 12.4 安全模型

当前仅做 sender endpoint 校验；**session_id 不是认证凭据**。当前协议假设 trusted LAN / trusted transport 环境，不提供密码学身份认证。

---

## 13. SessionManager

文件：

```text
include/aqua/session/session_manager.h
src/session/session_manager.cpp
```

存储：

```text
unordered_map<session_id, SessionInfo>
shared_mutex
```

状态只有：

```text
Created
Connected
```

session 创建：

```text
Connect RPC
  ↓
create_session()
  ↓
ID != 0
```

UDP HELLO：

```text
HELLO(session_id, sender)
  ↓
establish_session()
  ↓
endpoint = sender
  ↓
state = Connected
  ↓
last_seen = now
```

超时回收：

```text
remove_expired_sessions(timeout)
```

扫描、判断和删除在同一把 unique lock 下完成，避免扫描后再次判断产生 TOCTOU。

### 13.1 ID 生成

当前 session id 使用：

```text
16-bit instance id + 16-bit counter
```

ID 0 保留。

当前不承诺跨进程/跨机器稳定身份，也不承担认证功能。

---

## 14. gRPC Control Plane

### 14.1 Proto

文件：

```text
aqua_core/proto/aqua_service.proto
```

当前 service：

```text
Connect(ConnectRequest) → ConnectResponse
Disconnect(DisconnectRequest) → Empty
```

`ConnectResponse`：

```text
session_id
udp endpoint
audio_format
frame_count
```

其中 `frame_count = F` 是 Server 选定的 AudioFrame 长度，Client 用它预分配 JB。

### 14.2 GrpcClient

文件：

```text
include/aqua/net/grpc/grpc_client.h
src/net/grpc/grpc_client.cpp
```

职责：

1. 建立 channel；
2. 等待 server connected；
3. Connect RPC；
4. 校验 response；
5. 保存 `ConnectResult`；
6. Disconnect RPC。

校验至少包括：

```text
session_id != 0
udp address 非空
udp port 有效
audio_format valid
frame_count != 0
```

### 14.3 GrpcServer

文件：

```text
include/aqua/net/grpc/grpc_server.h
src/net/grpc/grpc_server.cpp
```

service implementation 直接使用 SessionManager：

```text
Connect
  ↓
create session
  ↓
return server format + frame_count + advertised UDP endpoint

Disconnect
  ↓
remove session
```

`GrpcServer::run()` 负责服务线程，`shutdown()` 负责关闭。

---

## 15.3 Timeline recovery

JitterBuffer 在 session 仍活跃但 sequence 脱离当前 playback window 时使用 Level 1 re-anchor：producer 只写入 `reanchor_request_seq_`，consumer 在 `pull()` 线程应用。触发帧仍尽可能正常进入 slot；应用时清理新窗口之外的 READY 槽，并复用已有 Fill-to-target 机制恢复播放。

HELLO_ACK 连续超时属于 Level 2 session failure，由 Runtime 进入 `Degraded`；Level 2 不与 JB 的正常 data-plane re-anchor 混用。

## 15. JitterBuffer：Client 唯一应用层音频缓冲

文件：

```text
include/aqua/audio/buffer/jitter_buffer.h
src/audio/buffer/jitter_buffer.cpp
```

JitterBuffer 是当前客户端播放模型的核心，不再存在独立 RingBuffer / watermark buffer / second audio buffer。

### 15.1 存储模型

固定 `N` 个 slot，每 slot 存一整帧：

```text
slot_bytes = F × frame_bytes
capacity_bytes = N × slot_bytes
```

slot 状态：

```text
Empty
Writing
Ready
```

producer：网络线程 `push()`。

consumer：Playback realtime thread `pull()`。

### 15.2 Sequence 定位

播放时序完全以 `AudioFrame::sequence` 为时间轴。

slot ring 只负责物理存储：

```text
slot_index = sequence % capacity
```

但是否接收由 sequence 与当前 `play_seq`、`highest_seq` 等状态判断。

### 15.3 Startup

默认：

```text
capacity = 30 slots
target = 60%
```

启动前只有在 timeline lead 达到 target 才建立播放 anchor。

注意：

```text
water_level = lead_slots / N
```

不是 physical `used_slots / N`。

因此：

```text
used_slots != water_level × N
```

是正常情况。

### 15.4 Watermark

阈值：

```text
warning_low  = 0.30
normal_low   = 0.45
target        = 0.60
normal_high  = 0.75
warning_high = 0.90
```

逻辑：

```text
低水位 warning
    ↓
Fill / hold
    ↓
逐步恢复至 target

高水位 warning
    ↓
Drop / skip
    ↓
逐步恢复至 target
```

`WarningStepFn` 是无状态 `noexcept` function pointer；不得在 pull path 创建对象、分配或加锁。

### 15.5 Pull

Playback callback 给出任意 `K` 帧 output，可能：

```text
K < F
K = F
K > F
```

JB 用 `read_offset_` 保留当前 slot 尚未消费的尾部，因此不要求 server callback size 与 client backend callback size 相同。

缺帧：

```text
当前 play_seq 对应 slot 不在 READY
    ↓
填 F 帧静音
    ↓
推进 playback timeline
```

耗尽时，JB 维持静音策略，不建立新的隐式 buffer。

### 15.6 并发原则

`pull()`：

```text
no mutex
no allocation
no system call
```

slot 的 `Ready` publish / `Empty` recycle 使用 acquire/release 原子状态。

`used_slots` 是诊断量，不用于决定实际播放时序。

### 15.7 Drift / adaptive correction 的边界

当前 JB 不直接实现时钟恢复控制器；未来 drift estimator / controller 应建立在：

```text
play_seq
occupancy / lead trend
packet arrival trend
PLC / loss statistics
```

而不是重新增加第二个 audio buffer。

---

## 16. ClientRuntime

文件：

```text
include/aqua/runtime/client_runtime.h
src/runtime/client_runtime.cpp
```

职责：composition root。

### 16.1 启动顺序

```text
Created
  ↓
Starting
  ↓
create DeviceManager
  ↓
create Playback
  ↓
gRPC connect
  ↓
obtain session_id / endpoint / format / F
  ↓
create JitterBuffer
  ↓
set UDP remote
  ↓
start UDP receive
  ↓
start HELLO
  ↓
start Playback
  ↓
Running
```

Audio callback 可以在 `Starting` 阶段被 backend 触发；最终进入 `Running` 后继续工作。

### 16.2 UDP → JB

UdpClient 只上报：

```text
sequence + PCM span
```

ClientRuntime 再组合：

```text
AudioFrame { sequence, F, pcm }
```

然后：

```text
jb->push(frame)
```

### 16.3 Playback callback

最终 callback：

```text
AudioPlayback
    ↓
ClientRuntime::pull_playback()
    ↓
JitterBuffer::pull()
```

### 16.4 错误状态

运行期 Playback backend event：

```text
AudioError
  ↓
last_audio_error
  ↓
Running/Starting → Degraded
```

当前 `Degraded` 不自动回 `Running`。

---

## 17. ServerRuntime

文件：

```text
include/aqua/runtime/server_runtime.h
src/runtime/server_runtime.cpp
```

它是 Server composition root。

### 17.1 启动顺序

```text
Created
  ↓
Starting
  ↓
validate format / frame_count / MTU / queue geometry
  ↓
create DeviceManager
  ↓
create Capture
  ↓
bind/start UDP
  ↓
start NetworkDispatcher
  ↓
start gRPC server
  ↓
start Capture
  ↓
Running
```

capture 在 `Starting` 阶段即允许将首帧送入 handoff queue，因此没有“capture 已经开始但 runtime bool 尚未置位”窗口。

### 17.2 Capture callback

实时路径严格固定：

```text
AudioBlock
  ↓
Packetizer::push
  ↓
AudioFrameQueue::push
  ↓
if accepted
    dispatcher.publish_from_realtime(should_notify)
```

禁止：

```text
SessionManager lock
NetworkFrame encode
shared_ptr fan-out
UDP enqueue
asio::post
logger I/O
```

### 17.3 Reap timer

Runtime 周期性执行：

```text
SessionManager::remove_expired_sessions(timeout)
```

timer handler 使用 weak capture，避免 Runtime 自持有。

### 17.4 停止顺序

```text
capture.stop()
    ↓
reap timer cancel
    ↓
dispatcher.stop()       ← join + final drain
    ↓
udp.stop()
    ↓
sessions.clear()
    ↓
grpc.shutdown()
    ↓
grpc thread join
    ↓
Stopped
```

这个顺序的目标是：停止产生新音频 → 消耗已经进入 handoff queue 的 frame → 关闭数据面 → 清 session → 关闭控制面。

---

## 18. Runtime Lifecycle

统一状态枚举：

```text
Created
Starting
Running
Degraded
Stopping
Stopped
```

允许路径：

```text
Created → Starting
Starting → Running
Starting → Degraded
Running → Degraded
Created/Starting/Running/Degraded → Stopping
Stopping → Stopped
```

`Degraded` 是 latch/terminal runtime state：当前版本不自动恢复。

### 18.1 `start()` / `stop()` 契约

`start()` 与 `stop()` 是 control-plane 操作，调用方应串行化，不要求 start/stop 相互并发安全。

`stop()` 自身通过 CAS 抢占 `Stopping`：

```text
多个 stop 调用
   ↓
只有一个线程获得 teardown ownership
其他直接 return
```

因此 stop 是幂等的。

### 18.2 错误传播

backend event thread 只做：

```text
store AudioError
CAS state → Degraded
log warning
```

不允许：

```text
event thread → stop()
```

真正 teardown 仍由 owner/control thread 负责。

---

## 19. Diagnostics 与日志

### 19.1 `Diagnostics`

文件：

```text
include/aqua/diagnostics/diagnostics.h
src/diagnostics/diagnostics.cpp
```

采用 source callback：

```text
name + SourceFn
```

调用 `print()` 时依次读取各 source。

Runtime/网络对象可通过 source 提供动态诊断。

### 19.2 Logger

文件：

```text
include/aqua/logger/logger.h
src/logger/logger.cpp
```

基于 spdlog，统一提供：

```text
trace
debug
info
warn
error
```

实时数据 callback 不应调用需要不可控 I/O 的 logger 路径；调试信息应尽量在 worker / control thread 记录。

### 19.3 当前 server audio accounting

建议诊断同时观察：

```text
runtime state
last audio error
capture running
frames encoded
frames broadcast
frames without clients
frames encode failed
frames dispatch failed
handoff dropped
UDP tx packets / errors
session count
```

这样能把：

```text
Audio domain
→ handoff
→ encoding
→ session fan-out
→ transport
```

逐层对账。

---

## 20. Address utilities

文件：

```text
include/aqua/net/address/address_utils.h
src/net/address/address_utils.cpp
```

职责：

- parse IP literal；
- 格式化 host:port；
- IPv4 / IPv6 方括号处理。

不承担 DNS、连接、socket 生命周期。

---

## 21. CLI

文件：

```text
aqua_app/cli/cli_parser/cli_parser_client.*
aqua_app/cli/cli_parser/cli_parser_server.*
aqua_app/cli/client_main.cpp
aqua_app/cli/server_main.cpp
```

CLI parser 负责：

- 参数合法性；
- 默认值；
- 字符串 → domain/config；
- 帮助信息。

Server CLI 当前重点配置：

```text
capture mode
capture device
format
frame count / derived packet size
UDP bind
RPC bind
advertised UDP address
network queue slots
session timeout / reap interval
```

Client CLI 当前重点配置：

```text
server address
RPC port
client name
playback device
JB capacity
HELLO interval
```

最终配置进入 Runtime；CLI 不自己实现音频或网络行为。

---

## 22. 线程模型总表

| 线程 | 组件 | 允许做什么 | 明确禁止 |
|---|---|---|---|
| Capture RT | WASAPI Capture | 获取 PCM、Packetizer、handoff push、atomic RMW/conditional wake | mutex、allocation、network、Asio、session lock |
| Network worker | AudioNetworkDispatcher | queue drain、wire encode、session snapshot、UDP enqueue | 直接操纵 WASAPI |
| Asio worker | UdpTransport / UdpClient / UdpServer / timers | socket async I/O、HELLO timer、receive handlers | 假设是 audio RT |
| Playback RT | WASAPI Playback | JB pull、PCM fill | mutex、allocation、I/O |
| gRPC thread | GrpcServer | RPC server loop | 直接参与 audio RT |
| Control/main | Runtime/CLI | start/stop、配置、诊断、join | 假装自己是 realtime thread |

---

## 23. Ownership 总表

### Server

```text
ServerRuntime
 ├── DeviceManager (unique_ptr)
 ├── Capture (unique_ptr)
 ├── SessionManager (shared_ptr)
 ├── UdpServer
 ├── AudioPacketizer
 ├── AudioFrameQueue
 ├── AudioNetworkDispatcher
 ├── GrpcServer (unique_ptr)
 ├── grpc thread
 └── reap timer
```

### Client

```text
ClientRuntime
 ├── DeviceManager (unique_ptr)
 ├── Playback (unique_ptr)
 ├── GrpcClient
 ├── UdpClient
 └── JitterBuffer (shared_ptr)
```

### UDP async state

`UdpTransport` / `UdpClient` / `UdpServer` 使用 `State` + async operation lifetime 管理；长生命周期 handler 优先使用 weak capture，避免对象通过自己的 handler 永久保持自己存活。

---

## 24. 网络协议与 session 时序

### 24.1 Connect

```text
Client
  │
  │ gRPC Connect(client_name)
  ▼
Server
  │ create_session()
  │
  │ response:
  │ session_id
  │ udp endpoint
  │ audio format
  │ frame_count F
  ▼
Client
```

### 24.2 UDP association

```text
Client
  │ HELLO(session_id)
  ▼
Server
  │ establish_session(id, sender)
  │
  │ HELLO_ACK(session_id)
  ▼
Client
```

之后 Client 周期性发送 HELLO，Server 更新 last_seen。Client 同时检查匹配 session_id 的 HELLO_ACK：连续 `HELLO_ACK_MISS_THRESHOLD` 个 interval 没有观察到新 ACK 时，由 liveness callback 通知 ClientRuntime；Runtime 决定进入 `Degraded`。单次正常 ACK 会清零连续 miss 计数。

Client 不以 AudioFrame inactivity 判断断连，因为远端合法静音也会造成一段时间没有音频数据。

### 24.3 Audio

```text
Server:
AudioFrame(sequence, pcm)
      ↓
NetworkFrame(Audio)
      ↓
UDP

Client:
UDP
  ↓
NetworkFrame decode
  ↓
(sequence, pcm)
  ↓
AudioFrame(sequence, F, pcm)
  ↓
JB.push()
```

### 24.4 Disconnect

正常关闭：

```text
ClientRuntime.stop()
  ↓
playback stop
  ↓
UDP stop
  ↓
gRPC Disconnect(session_id)
```

Server 的 timeout 机制用于异常断线兜底，而不是正常 Disconnect 的首选路径。

---

## 25. Backpressure / Drop 策略

系统存在多个不同层次的容量，不能混为一个“buffer”：

### 25.1 WASAPI backend buffer

由 Windows audio engine/backend 管理；决定 callback 粒度/设备侧 latency。

### 25.2 Server AudioFrameQueue

```text
RT → network handoff
```

默认 4 frame；满时 drop newest；不参与播放 latency。

### 25.3 UDP pending datagram queue

```text
network dispatcher → socket async_send
```

默认 64 datagrams；满时 drop oldest pending packet；只处理 transport burst，不是 audio clock buffer。

### 25.4 Client JitterBuffer

```text
network → playout timeline
```

这是唯一参与音频播放时序的 application-level buffer。

---

## 26. 测试结构

### 26.1 Audio

```text
audio_format_test
jitter_buffer_test
jitter_buffer_boundary_test
audio_frame_queue_test
packetizer/audio_packetizer_test
packetizer/audio_packetizer_boundary_test
wasapi_audio_capture_test
wasapi_audio_playback_test
wasapi_device_manager_test
```

覆盖：

- PCM format invariant；
- JB sequence / startup / Fill / Drop；
- slot / callback 边界；
- queue SPSC；
- packetizer 重切；
- WASAPI backend 集成行为。

### 26.2 Network / Runtime

```text
address_utils_test
address_edge_cases_test
grpc_client_test
grpc_server_test
grpc_edge_cases_test
grpc_audio_format_converter_test
network_frame_test
network_frame_boundary_test
udp_protocol_test
udp_edge_cases_test
udp_loopback_test
runtime_state_test
audio_network_dispatcher_test
```

重点覆盖：

- IPv4 / IPv6 地址；
- gRPC connect/disconnect；
- wire encode/decode；
- UDP HELLO；
- UDP loopback；
- Runtime lifecycle；
- broadcast accounting；
- dispatcher wake / stop-drain。

### 26.3 Diagnostics / Logger

分别验证 source collection、打印以及日志等级和 logger 生命周期。

---

## 27. 当前必须保持的 Invariants

这些条件属于“架构冻结规则”：

### Audio

```text
AudioFrame.frame_count == negotiated F
AudioFrame.data.size() == F × frame_bytes
sequence 只由 AudioPacketizer 产生
```

### Server RT

```text
capture callback
    = Packetizer + AudioFrameQueue + atomic wake protocol
```

除此之外不允许加入网络工作。

### Queue wake

```text
每次 accepted push:
    generation++

仅 should_notify:
    notify_one()
```

worker 必须：

```text
drain
→ empty check
→ generation load
→ queue recheck
→ wait(observed)
```

### Network layer

```text
UdpTransport       不知道 AudioFrame
UdpServer          不知道 AudioFrame
UdpClient          不知道 AudioFrame
```

Audio domain → network domain 的唯一桥接在 dispatcher/runtime 层。

### Client buffering

```text
JitterBuffer = 唯一 application-level playout buffer
```

不要重新引入独立 RingBuffer 或第二套 watermark buffer。

### Lifecycle

```text
Degraded 不自动回 Running
stop 幂等
backend event callback 不直接 stop Runtime
```

### Protocol

```text
session_id 是 identifier，不是 authenticator
frame_count 不进 Audio UDP packet
UDP audio payload <= UDP_AUDIO_PAYLOAD_BYTES
```

---

## 28. 当前已知限制 / 明确暂缓

1. **身份认证尚未实现。** 当前 session_id + HELLO 只能做协议关联，不能防止知道 session_id 的攻击者伪造 HELLO，也不能提供加密/完整性保护。
2. **默认设备运行中跟随尚未实现。** v1 在 start 时解析一次，设备断开进入 Degraded。
3. **Degraded 没有自动恢复。** 后续如需要设备重建，应新增显式 recovery 生命周期，而不是偷偷从 Degraded 回 Running。
4. **Client 当前只接受配置 remote endpoint 的 Audio datagram。** 未来出现 relay / multipath / NAT rebinding 后需要更正式的 data-plane identity。
5. **每 frame snapshot session 的模型适合当前 client 数量。** 大规模 client 场景可再引入 immutable endpoint snapshot；当前不提前增加复杂度。
6. **UDP broadcast 当前使用单一全局 pending queue。** 极大规模、多弱网 client 场景可演进成 per-client queue；当前 transport 规模下不需要。
7. **Server handoff queue 满时 drop newest。** 这是当前有意选择的低延迟策略；若未来需要 drop-oldest，需要重新设计 SPSC overwrite 所有权协议，不能简单移动 cursor。
8. **AudioFormat conversion / resampling 尚未并入当前 playback 核心路径。** 当前 client 期望播放 format 与 server stream format 一致；转换能力通过独立 converter 接口预留。

---

## 29. 推荐的验证顺序

每次修改底层代码后，按以下顺序验证：

```text
1. compile
2. domain unit tests
3. queue / JB concurrency tests
4. NetworkFrame / UDP protocol tests
5. Runtime lifecycle tests
6. full CTest
7. Windows WASAPI integration tests
8. CPU / compiler-load stress
9. long-run audio soak
```

特别是低延迟问题，不应只观察“没有 crash”。还需要同时看：

```text
capture callback stability
handoff dropped_frames
network tx queue depth
JB used_slots / water_level
JB underrun / PLC statistics
audio end-to-end latency
estimated drift
```

---

## 30. 文件地图（当前实现）

### `include/aqua/audio`

```text
audio_block.h                 变长 capture PCM block
audio_error.h                 跨 backend/runtime 错误
audio_format.h                PCM format + byte/frame 计算
audio_format_converter.h      protobuf ↔ AudioFormat
audio_frame.h                 定长带 sequence 的 audio frame

buffer/jitter_buffer.h        Client 唯一 playout buffer
capture/audio_capture.h       Capture 抽象
capture/audio_capture_config.h Capture 配置
devices/audio_device.h        Device / Direction / ID
devices/audio_device_manager.h Device enumerate/resolve
packetizer/audio_packetizer.h 变长 block → 定长 frame
playback/audio_playback.h     Playback 抽象
playback/audio_playback_config.h Playback 配置
queue/audio_frame_queue.h     RT → network SPSC handoff
```

### `src/audio`

```text
audio_format_converter.cpp
audio/buffer/jitter_buffer.cpp
capture/audio_capture_factory.cpp
capture/wasapi/wasapi_audio_capture.cpp
capture/wasapi/wasapi_audio_capture.h
devices/audio_device_manager_factory.cpp
devices/wasapi/wasapi_device_manager.cpp
devices/wasapi/wasapi_device_manager.h
packetizer/audio_packetizer.cpp
playback/audio_playback_factory.cpp
playback/wasapi/wasapi_audio_playback.cpp
playback/wasapi/wasapi_audio_playback.h
wasapi/wasapi_com.h
```

### `include/aqua/net`

```text
address/address_utils.h

grpc/grpc_client.h
grpc/grpc_config.h
grpc/grpc_include.h
grpc/grpc_server.h

udp/network_frame.h
udp/udp_client.h
udp/udp_config.h
udp/udp_server.h
udp/udp_transport.h
```

### `src/net`

对应实现：

```text
address/address_utils.cpp
grpc/grpc_client.cpp
grpc/grpc_server.cpp
udp/network_frame.cpp
udp/udp_client.cpp
udp/udp_server.cpp
udp/udp_transport.cpp
```

### `include/aqua/runtime`

```text
runtime_state.h
server_runtime.h
client_runtime.h
audio_network_dispatcher.h
```

### `src/runtime`

```text
server_runtime.cpp
client_runtime.cpp
audio_network_dispatcher.cpp
```

### Session / infrastructure

```text
session/session_manager.h
src/session/session_manager.cpp

diagnostics/diagnostics.h
src/diagnostics/diagnostics.cpp

logger/logger.h
src/logger/logger.cpp
```

### Application

```text
aqua_app/cli/client_main.cpp
aqua_app/cli/server_main.cpp
cli_parser/cli_parser_client.*
cli_parser/cli_parser_server.*
cli_version.h.in
```

### Design docs

```text
doc/audio_design.md
    音频 domain + backend + realtime + runtime 设计原则

doc/buffer_design.md
    JB-only、slot、水位、callback 粒度、并发契约

doc/architecture.md
    本文：整个项目的实现级架构总览

doc/REFACTOR_NOTES_V10.md
    当前架构硬化、JB re-anchor 与 HELLO_ACK liveness 记录
```

---

## 31. 架构冻结结论

当前结构已经可以视为冻结候选：

```text
Capture backend
    ↓
AudioBlock
    ↓
Packetizer
    ↓
AudioFrameQueue
    ↓
AudioNetworkDispatcher
    ↓
NetworkFrame
    ↓
UdpServer / UdpClient / UdpTransport
    ↓
AudioFrame
    ↓
JitterBuffer
    ↓
Playback backend
```

未来增加功能时，应优先在既有边界内扩展，而不是重新增加跨层依赖。

尤其避免重新引入：

```text
AudioFrame → UdpTransport
AudioCapture RT → Asio
AudioCapture RT → SessionManager
JB → RingBuffer
Runtime → backend realtime loop
```

这些都是已经明确验证为不理想的方向。

---

## 32. 本轮 v6 修改摘要

相对 v4：

```text
AudioFrameQueue::PushResult
    wake hint（publish 后重新读取 tail） → should_notify

producer:
    accepted push
      → generation.fetch_add()
      → if should_notify: notify_one()

worker:
    保留 drain → generation load → queue recheck → wait(observed)
```

目的：

- 保留 generation-based wait 协议；v6 将 should_notify 改为基于 publish 后 cursor 的更精确 wake hint；
- 不让 capture realtime callback 每帧执行 unconditional wake primitive；
- 将 `should_notify` 明确为 wake hint 而不是 queue-state truth；
- 文档、实现、测试三者使用同一个同步模型。


### UDP send accounting

`tx_errors` excludes expected `operation_aborted` caused by normal shutdown. Pending-queue publication/scheduling failures are counted as `tx_enqueue_failures`; when a newly
required send-pump task cannot be scheduled, all pending datagrams are explicitly dropped rather
than left in a non-progressing queue.


### ClientRuntime asynchronous callback lifetime

ClientRuntime owns a small shared callback lifetime gate. UDP receive/liveness handlers may outlive
the Runtime object briefly while the io context drains; callbacks acquire the gate before invoking
Runtime methods. Runtime destruction detaches the gate before member destruction, so no asynchronous
UDP callback can dereference a destroyed ClientRuntime.

### Client liveness terminal semantics

Three consecutive HELLO_ACK miss intervals transition ClientRuntime to terminal `Degraded`. This is
a session/control-plane fault indication, not an automatic reconnect. The current one-shot Runtime
contract requires the owner to stop/recreate the Runtime for a new session.


### Session restart boundary

The current ClientRuntime is one-shot and does not automatically reconnect after Level 2 liveness
failure. A newly established session must begin a new JitterBuffer episode; an old `play_seq_` must
never be carried across a new session because the server-side AudioPacketizer sequence restarts with
the new process/session lifecycle.

### Packetizer diagnostics

`AudioPacketizer::rejected_unaligned_blocks()` counts capture blocks rejected because their byte size
is not an integer number of sample frames. It is a diagnostic counter only and does not log from the
realtime capture path. `reset()` clears it together with the packetizer sequence for a new capture episode.
