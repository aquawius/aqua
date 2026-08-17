# 架构设计

> 目标：在 Windows / Linux / Android 之间，以足够低的延迟将一台设备的音频实时传输到另一台设备回放。
> 第一阶段以 **PCM + UDP + gRPC + 一层 NAT 穿透** 为核心，优先验证低延迟链路与 NAT 环境下的可靠连接。

## 1. 技术栈

| 层级 | 技术 | 说明 |
|---|---|---|
| 语言 | C++23 | CMake 强制 `CXX_STANDARD=23` |
| 构建 | CMake ≥ 4.2 | 配合 vcpkg manifest |
| 包管理 | vcpkg | 依赖锁定在 `vcpkg.json` |
| 异步网络 | Asio（独立版） | UDP 数据面 |
| 控制面 | gRPC + protobuf | Connect / Disconnect |
| Windows 音频 | WASAPI | 采集 / 播放 |
| Android 音频 | AAudio | 播放（采集后续） |
| 桌面 UI | Qt6（后续） | — |
| Android UI | Kotlin + JNI | — |
| 缓冲 | SPSC RingBuffer | 音频线程间传输 |
| Jitter Buffer | 自定义 | playout deadline |
| 日志 | spdlog | 核心库日志 |
| 命令行 | cxxopts | CLI 解析 |
| 测试 | GoogleTest | 单元 / 集成 |
| Codec | PCM | 当前唯一格式（Opus 后续） |

## 2. 分层架构

```text
┌──────────────────────────────────────────┐
│                  UI Layer                 │
│   Windows/Linux: Qt6  Android: Kotlin     │
└──────────────────────┬───────────────────┘
                       │ C API (aqua.h)
                       ▼
┌──────────────────────────────────────────┐
│                Core Library               │
│   Control Plane: gRPC client/server       │
│   Audio Pipeline: backend/ringbuffer/PCM  │
│   SessionManager (session 状态)           │
└──────────────────────────────────────────┘
```

核心库不得依赖 Qt / Android SDK / Kotlin / WASAPI / PipeWire / AAudio；平台代码必须封装在 Audio Backend 中。

## 3. 核心设计原则

### 控制面与数据面分离

```text
Control Plane:  gRPC -> SessionManager -> UDP endpoint
Data Plane:     UDP  -> Audio Stream
```

- gRPC：建立 session、返回 session_id + UDP endpoint + AudioFormat、Disconnect。
- UDP：NAT 探测、NAT 映射、音频数据、HELLO 保活（刷新 NAT 映射 + server session last_seen）。
- **gRPC 不承载音频、不参与保活**，保活完全由 UDP HELLO 完成。

## 4. 音频数据流

### Sender

```text
WASAPI/PipeWire/AAudio -> Audio Callback -> SPSC RingBuffer -> Packetizer -> AudioPacket -> UDP Send
```

### Receiver

```text
UDP Receive -> Packet Parse -> Sequence Check -> Jitter Buffer -> PCM Buffer -> Audio Backend
```

## 5. 线程模型

| 线程 | 职责 | 约束 |
|---|---|---|
| Audio Backend | 从 RingBuffer 读/写 PCM | 无锁、无分配、无阻塞、无网络 I/O |
| UDP I/O | `io_context.run()`：收发 + JitterBuffer push/pop + HELLO keepalive timer | 单线程 run，避免锁竞争 |
| Control | gRPC Completion Queue / 同步 RPC | 与控制面隔离 |

> Session 清理（server）与 HELLO keepalive（client）都已改为 `io_context` 上的 `steady_timer`，与 UDP 收发串行。

### 跨线程通信路径

```text
Capture Thread --SPSC RingBuffer--> UDP I/O Thread --UDP--> 远端
远端 --UDP--> UDP I/O Thread(JB.push) -> steady_timer -> JB.pop_next --SPSC RingBuffer--> Playback Thread
gRPC Thread --SessionManager(shared_mutex)--> UDP I/O Thread
```

允许跨线程共享：`SessionManager`（shared_mutex）、`SpscRingBuffer`（无锁 SPSC）、`std::atomic` 标志。
禁止：直接共享 `asio::udp::socket`（须 `post`）、音频回调访问 SessionManager、UDP 回调里调阻塞 gRPC。

## 6. 低延迟原则

- 实时音频必须用 **UDP**，不用 TCP（避免重传队头阻塞）。
- 热路径（Audio Callback / UDP 收发 / Jitter Buffer）尽量：无动态分配、少锁、预分配 buffer、`std::span`、固定大小 packet buffer。

## 7. 核心设计总结

> **gRPC 管连接，SessionManager 管状态，UDP 管音频，Audio Backend 管设备，Client 管自己的格式转换。**

Server 是「纯网络转发 + Session 管理」，不参与音频格式转换，也不承担音频设备逻辑。

## 8. 模块依赖图

```text
main (exe) / Qt6 / Android C API
        │
        ▼
   aqua_core（编排层 server_runtime/client_runtime 依赖所有组件）
        │
        ▼
   aqua_proto
        │
        ▼
 asio  spdlog  gRPC  protobuf  cxxopts
```

依赖方向规则：

- `main`（CLI）→ `aqua_core` → `aqua_proto`（单向，上层依赖下层）。
- 未来 `Qt6 / Android UI` → `aqua_capi` → `aqua_core`（C API 是 UI 唯一入口）。
- `server_runtime` / `client_runtime`（编排层）依赖所有组件；组件不得反向依赖编排层。
- `logger` 是横切关注点，任何模块可依赖，但不得反向依赖业务模块。
- `audio/backend` 只通过抽象接口被使用，平台实现互不可见。
- `session_manager` 是纯状态容器，不依赖 net / grpc / audio。

## 9. 并发模型

### 关闭顺序

**Server**：capture.stop → grpc.shutdown → transport.stop → ioc.stop → join → sessions.clear。
**Client**：playback.stop → grpc.disconnect（先通知 server 停止发包，避免 ICMP 风暴）→ transport.stop → ioc.stop → join。

主循环每 50ms 轮询健康标志：`capture/playback->is_running()`、`grpc_server.is_running()`（server）、数据接收超时（client）。

## 10. 错误处理策略

| 层 | 策略 |
|---|---|
| 音频回调 | 不抛异常；失败静默丢弃/填零，记 trace |
| UDP 收发 | 不抛异常；解码失败丢包，send 失败记 warn |
| net/packet | 返回 `std::optional` |
| SessionManager | 返回 `bool` / `optional` |
| gRPC | 用 `grpc::Status` 返回错误 |
| C API 边界 | `try/catch` 全捕获，返回负码 |
| main | 解析失败打印 stderr 并返回非零 |

仅以下场景允许终止进程：proto 与原生编码数值不一致（启动 assert）、io_context 启动失败、致命 bind 失败。其余一律降级处理，不 crash。

### 客户端断连恢复

- Server 侧：超过 `SESSION_TIMEOUT`（5s）未收 HELLO → `remove_session`。
- Client 侧：超过 `CLIENT_AUDIO_RECV_TIMEOUT`（5s）未收 Audio → 认为 server 已断开。
- `--auto-reconnect`（默认关）启用指数退避重连（1/2/4/8/16/30s，稳定 30s 后重置退避）。
