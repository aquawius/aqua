# Aqua — 底层架构与接口设计

> 目标：在 Windows / Linux / Android 等主流平台之间，以足够低的延迟将一台设备的音频实时传输到另一台设备回放。
>
> 核心使用现代 C++20，CMake 构建；UI 与核心完全解耦。
>
> 当前第一阶段以 **PCM + UDP + gRPC + 一层 NAT 穿透** 为核心目标，优先验证低延迟音频链路和 NAT 环境下的可靠连接。

---

# 0. 项目目标与当前原则

## 0.1 项目目标

Aqua 是一个跨平台低延迟网络音频共享系统：

- Windows
- Linux
- Android
- 后续可扩展 macOS

核心目标：

- 低延迟音频采集
- UDP 实时传输
- 稳定低延迟播放
- 支持 NAT 环境
- 核心库与 UI 完全解耦
- 控制面与数据面彻底分离

---

## 0.2 当前阶段目标

当前优先实现：

1. Windows WASAPI 音频采集
2. PCM 数据封装
3. UDP Unicast 传输
4. Windows WASAPI 音频播放
5. gRPC 控制连接
6. SessionManager
7. 一层 NAT UDP Hole Punching
8. 基础序列号与丢包处理

当前阶段**不实现**：

- Opus
- 多 Codec 协商
- UDP Multicast
- STUN
- TURN
- 复杂 ICE
- 多人房间
- 云端身份系统
- Server 端音频格式转换
- Server 端重采样

---

## 0.3 核心设计原则

### 控制面与数据面分离

```text
                    Control Plane
                         |
                       gRPC
                         |
                         v
                  Session Manager
                         |
                         |
                         v
                    UDP Endpoint


                    Data Plane
                         |
                        UDP
                         |
                         v
                    Audio Stream
```

gRPC：

- 建立 session
- 返回 session_id
- 返回 UDP endpoint
- 返回服务器 AudioFormat
- KeepAlive
- Disconnect

UDP：

- NAT 探测
- NAT 映射
- 音频数据
- 后续序列号 / 同步信息

**gRPC 不承载音频数据。**

---

# 1. 技术栈

| 层级 | 技术 | 说明 |
|---|---|---|
| 语言 | C++20 | 核心实现 |
| 构建 | CMake | 跨平台构建 |
| 异步网络 | Asio / Boost.Asio | UDP 数据面 |
| 控制面 | gRPC + protobuf | Session 控制 |
| Windows 音频 | WASAPI | 采集 / 播放 |
| Linux 音频 | PipeWire | 后续实现 |
| Android 音频 | AAudio | 后续实现 |
| 桌面 UI | Qt6 | Windows / Linux |
| Android UI | Kotlin + JNI | Android |
| 缓冲 | SPSC RingBuffer | 音频线程之间传输 |
| Jitter Buffer | 自定义 | 后续加入 |
| Codec | PCM | 当前阶段唯一格式 |
| Codec | Opus | 后续阶段 |

---

# 2. 分层架构

```text
┌──────────────────────────────────────────────────────────┐
│                         UI Layer                          │
│                                                          │
│ Windows / Linux: Qt6                                     │
│ Android: Kotlin                                          │
└──────────────────────────┬───────────────────────────────┘
                           │
                           │ C API
                           v
┌──────────────────────────────────────────────────────────┐
│                     Core Library                         │
│                                                          │
│  ┌─────────────────┐      ┌─────────────────────────┐   │
│  │ Control Plane   │      │      Audio Pipeline     │   │
│  │                 │      │                         │   │
│  │ gRPC Client     │      │ Audio Backend           │   │
│  │ gRPC Server     │      │ RingBuffer              │   │
│  │                 │      │ PCM                     │   │
│  └────────┬────────┘      │ UDP                     │   │
│           │               └────────────┬────────────┘   │
│           │                            │                │
│           v                            v                │
│  ┌──────────────────────────────────────────────────┐  │
│  │                  SessionManager                  │  │
│  │                                                  │  │
│  │ session_id                                       │  │
│  │ UDP NAT endpoint                                 │  │
│  │ created_at                                       │  │
│  │ last_seen                                        │  │
│  │ connected                                        │  │
│  └──────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

核心库不得依赖：

- Qt
- Android SDK
- Kotlin
- WASAPI
- PipeWire
- AAudio

平台相关代码必须封装在 Audio Backend 中。

---

# 3. Audio Format

## 3.1 Server 固定 AudioFormat

Server 启动时确定一个 AudioFormat。

例如：

```text
PCM S16LE
48000 Hz
2 channels
```

Server 运行期间 AudioFormat 不发生变化。

Client 通过 `Connect` 获取服务器 AudioFormat。

---

## 3.2 Server 不进行音频转换

Server 不负责：

- 重采样
- 声道转换
- Sample Format 转换
- Codec 转换

例如 Server：

```text
PCM S16LE
48000 Hz
2 channels
```

Client 如果只能处理：

```text
PCM F32LE
44100 Hz
2 channels
```

由 Client 自己完成：

```text
Server
  |
  | PCM S16LE / 48kHz
  v
Client Network
  |
  v
Client Conversion
  |
  | PCM F32LE / 44.1kHz
  v
Audio Backend
```

这样 Server 数据面保持简单，不承担额外 CPU 开销。

---

## 3.3 AudioFormat 定义

当前 protobuf：

```proto
message AudioFormat {

  enum Encoding {

    ENCODING_INVALID = 0;

    // PCM Signed 16-bit Little Endian
    ENCODING_PCM_S16LE = 1;

    // PCM Signed 32-bit Little Endian
    ENCODING_PCM_S32LE = 2;

    // PCM Float 32-bit Little Endian
    ENCODING_PCM_F32LE = 3;

    // PCM Signed 24-bit Little Endian
    ENCODING_PCM_S24LE = 4;

    // PCM Unsigned 8-bit
    ENCODING_PCM_U8 = 5;
  }

  Encoding encoding = 1;

  uint32 channels = 2;

  uint32 sample_rate = 3;
}
```

`AudioFormat` 只描述**音频数据本身**。

不要在 AudioFormat 中加入：

```text
frame_samples
packet_size
jitter_buffer_size
target_latency
```

这些属于传输层或播放策略，而不是 AudioFormat。

---

# 4. gRPC Control Plane

## 4.1 gRPC 职责

当前只提供：

```text
Connect
KeepAlive
Disconnect
```

不提供：

```text
GetAudioFormat
SetAudioFormat
Codec negotiation
RegisterMediaEndpoint
```

因为：

- Server AudioFormat 固定
- Connect 已经返回 AudioFormat
- UDP endpoint 不需要通过第二次 RPC 注册
- Client 直接向 Server UDP endpoint 发送探测包即可完成 NAT 注册

---

## 4.2 Connect

Client：

```text
ConnectRequest
```

Server：

```text
ConnectResponse
```

返回：

```text
session_id
udp endpoint
audio format
```

概念上：

```text
Client
  |
  | gRPC Connect
  v
Server
  |
  | session_id
  | UDP endpoint
  | AudioFormat
  v
Client
```

---

## 4.3 Session ID

Session ID 只用于：

- 区分当前 Server 进程中的 Session
- UDP 数据包路由
- SessionManager 查表

不作为：

- 用户身份
- 长期设备 ID
- 认证凭据
- 加密 Token

使用：

```cpp
using session_id_t = std::uint32_t;
```

不使用 UUID。

Session ID 只需要在当前 Server 生命周期内尽可能避免冲突。

推荐使用：

```text
16 bit random instance
+
16 bit counter
```

例如：

```text
7A31-0001
7A31-0002
7A31-0003
```

内部仍然是：

```cpp
uint32_t
```

这样既节省网络空间，又方便日志和调试。

---

# 5. SessionManager

SessionManager 只负责 Session 生命周期和 UDP NAT endpoint。

核心结构：

```cpp
class SessionManager {
public:

    using session_id_t = std::uint32_t;

    struct SessionInfo {

        session_id_t session_id;

        // UDP NAT 映射地址
        asio::ip::udp::endpoint endpoint;

        // 创建时间
        std::chrono::steady_clock::time_point created_at;

        // 最近一次 UDP 通信
        std::chrono::steady_clock::time_point last_seen;

        // 是否完成 UDP 握手
        bool connected = false;
    };

};
```

SessionManager 不负责：

- 音频采集
- 音频播放
- PCM 转换
- UDP socket 生命周期
- gRPC 生命周期
- Codec

它只维护：

```text
session_id
    |
    +-- endpoint
    +-- created_at
    +-- last_seen
    +-- connected
```

---

# 6. NAT Traversal

## 6.1 当前目标

只实现：

```text
Client behind NAT
        |
        | UDP
        v
Public Server
```

即：

**Client 位于一层 NAT 后，Server 具有公网 UDP 地址。**

暂时不实现：

- 双方 NAT
- 对称 NAT
- STUN
- TURN
- 完整 ICE

---

## 6.2 Server 端口

Server 使用：

```text
TCP / gRPC
UDP / Media
```

例如：

```text
TCP 50051
UDP 50000
```

UDP 使用一个固定端口。

**不要为每个 Session 分配 UDP 端口。**

```text
Session A ─┐
Session B ─┼──> UDP :50000
Session C ─┘
```

Server 根据：

```text
session_id
```

查找 Session。

---

## 6.3 NAT 建立流程

```text
             Client
               |
               |
         gRPC Connect
               |
               v
             Server
               |
               |
       session_id = 0x12345678
       UDP = server:50000
               |
               v
             Client


             UDP
               |
               | HELLO + session_id
               v
              NAT
               |
               v
             Server
               |
               | 记录 source endpoint
               |
               v
        SessionManager
```

Server 收到 UDP：

```text
HELLO
session_id
```

然后：

```cpp
SessionManager::update_endpoint(
    session_id,
    sender_endpoint
);
```

保存：

```text
session_id
        ->
NAT mapped IP:port
```

---

## 6.4 UDP 握手响应

Server 收到合法 HELLO 后立即回复：

```text
HELLO_ACK
session_id
```

Client 收到 ACK 后认为 UDP 通道建立。

SessionManager：

```text
connected = true
```

---

## 6.5 NAT endpoint 不由 gRPC 提供

不要：

```text
Client
  |
  | gRPC RegisterMediaEndpoint
  v
Server
```

因为 Client 只能告诉 Server 自己的本地地址：

```text
192.168.1.100:12345
```

Server 真正需要的是 NAT 映射后的：

```text
203.0.113.10:54321
```

因此：

**Server 必须以 UDP packet 的实际 source endpoint 为准。**

---

# 7. UDP Packet

## 7.1 基本原则

UDP 不使用 protobuf。

音频数据走自定义二进制 packet。

原因：

- 减少包头
- 减少 CPU 开销
- 避免 protobuf allocation
- 更容易控制热路径
- 更适合预分配 buffer

---

## 7.2 Audio Packet

第一阶段建议：

```cpp
struct AudioPacketHeader
{
    std::uint32_t session_id;

    std::uint32_t sequence;

    std::uint32_t sample_position;

    std::uint16_t payload_size;
};
```

后面紧跟：

```text
PCM payload
```

整体：

```text
+------------------+
| session_id       |
+------------------+
| sequence         |
+------------------+
| sample_position  |
+------------------+
| payload_size     |
+------------------+
| PCM data         |
+------------------+
```

---

## 7.3 session_id

Server：

```cpp
auto it = sessions.find(packet.session_id);
```

不存在：

```text
discard
```

存在：

```text
touch_session(session_id)
```

---

## 7.4 sequence

`sequence` 用于：

- 判断重复 packet
- 判断 packet 顺序
- 判断丢包
- Jitter Buffer 排序

例如：

```text
100
101
102
104
```

可以检测：

```text
103 lost
```

UDP 不进行重传。

---

## 7.5 sample_position

`sample_position` 是音频时间轴。

例如：

48kHz：

```text
Packet 0:
sample_position = 0

Packet 1:
sample_position = 480

Packet 2:
sample_position = 960
```

如果每包 10ms：

```text
480 samples
```

但 **10ms / 480 samples 不属于 AudioFormat**。

发送端可以根据实际 packetization 策略决定每个 packet 包含多少 sample。

---

## 7.6 payload_size

接收端根据：

```text
AudioFormat
+
payload_size
```

计算实际 sample 数。

例如：

```text
PCM S16LE
2 channels

bytes_per_sample = 2

sample_count =
payload_size / (2 * channels)
```

---

# 8. UDP Control Packet

NAT 探测包和音频包不要混淆。

可以定义简单 packet type：

```cpp
enum class PacketType : std::uint8_t
{
    Hello = 1,
    HelloAck = 2,
    Audio = 3,
};
```

例如：

```text
HELLO

+----------+
| type     |
+----------+
| session  |
+----------+
```

音频：

```text
AUDIO

+----------+
| type     |
+----------+
| session  |
+----------+
| sequence |
+----------+
| position |
+----------+
| size     |
+----------+
| PCM      |
+----------+
```

---

# 9. 音频数据流

## 9.1 Sender

```text
WASAPI / PipeWire / AAudio
          |
          v
     Audio Callback
          |
          v
     SPSC RingBuffer
          |
          v
     Packetizer
          |
          v
     AudioPacket
          |
          v
       UDP Send
```

---

## 9.2 Receiver

```text
       UDP Receive
            |
            v
       Packet Parse
            |
            v
       Sequence Check
            |
            v
      Jitter Buffer
            |
            v
       PCM Buffer
            |
            v
       Audio Backend
```

---

# 10. 线程模型

## Audio Backend Thread

职责：

- WASAPI / PipeWire / AAudio callback
- 从 RingBuffer 读取或写入 PCM

约束：

- 不进行网络 I/O
- 不进行阻塞操作
- 不进行动态内存分配
- 不进行复杂计算

---

## UDP Thread

使用：

```cpp
asio::io_context
```

负责：

- UDP receive
- UDP send
- HELLO
- HELLO_ACK
- AudioPacket 接收
- AudioPacket 发送

数据面必须尽量避免：

- 阻塞
- 动态分配
- 大量锁竞争

---

## Control Thread

负责：

- gRPC Completion Queue
- Connect
- KeepAlive
- Disconnect

控制面和 UDP 数据面完全隔离。

---

# 11. RingBuffer

音频线程与普通线程之间使用：

```text
SPSC RingBuffer
```

原则：

```text
Audio Callback
      |
      v
SPSC RingBuffer
      |
      v
Network / Packetizer
```

不要让 Audio Callback 直接操作：

- UDP socket
- gRPC
- SessionManager mutex
- Jitter Buffer
- Codec

---

# 12. Jitter Buffer

第一阶段可以先使用简单 Buffer。

目标：

- packet 排序
- packet 去重
- 检测丢包
- packet 缺失时填充静音
- 控制播放延迟

后续再实现 Adaptive Jitter Buffer。

---

# 13. Clock Synchronization

音频同步不能依赖 wall clock。

不使用：

```text
NTP
```

而使用：

```text
sample_position
+
steady_clock
```

音频时间轴以 sample 为基础。

后续可以增加：

```text
Sender Report
Receiver Report
```

用于：

- 网络延迟估计
- clock drift
- sample rate drift

最终通过 resampler 进行微调。

---

# 14. Client Audio Format Conversion

Server 永远发送固定 AudioFormat。

Client 在接收之后决定是否需要转换。

例如：

```text
Server:

S16LE
48000
2ch

        |
        v

Client

S16LE
48000
2ch

        |
        v

Audio Backend
```

或者：

```text
Server:

S16LE
48000
2ch

        |
        v

Client Converter

F32
44100
2ch

        |
        v

Audio Backend
```

Server 不参与转换。

这样保证：

```text
Server
=
纯网络转发
+
Session 管理
```

不会因为不同 Client 的音频设备而增加复杂度。

---

# 15. 低延迟原则

## 15.1 UDP

实时音频必须使用 UDP。

不要使用：

```text
TCP
```

作为音频数据传输协议。

原因：

TCP 重传和队头阻塞会导致：

```text
packet loss
    |
    v
TCP retransmission
    |
    v
waiting
    |
    v
audio latency increase
```

实时音频宁可丢掉一个 packet，也不要为了一个 packet 阻塞整个音频流。

---

## 15.2 热路径

以下路径尽量：

- 无动态内存分配
- 少锁
- 使用预分配 buffer
- 使用 `std::span`
- 使用固定大小 packet buffer

尤其是：

```text
Audio Callback
UDP Receive
UDP Send
Jitter Buffer
```

---

# 16. 目录结构

```text
aqua/
├── CMakeLists.txt
├── AGENT.md
│
├── include/
│   └── audio_share.h
│
├── proto/
│   └── aqua_service.proto
│
├── src/
│   ├── core/
│   │   ├── public/
│   │   │
│   │   ├── logger/
│   │   │   ├── logger.h
│   │   │   └── logger.cpp
│   │   │
│   │   ├── audio/
│   │   │   ├── backend/
│   │   │   │   ├── wasapi/
│   │   │   │   ├── pipewire/
│   │   │   │   └── aaudio/
│   │   │   │
│   │   │   └── ringbuffer/
│   │   │
│   │   ├── net/
│   │   │   ├── transport/
│   │   │   ├── packet/
│   │   │   └── nat/
│   │   │
│   │   ├── session
│   │   │   ├── session_manager.h
│   │   │   └── session_manager.cpp
│   │   │
│   │   ├── grpc/
│   │   │   ├── grpc_server.cpp
│   │   │   └── grpc_client.cpp
│   │   │
│   │   └── jitter_buffer/
│   │
│   ├── main/
│   │   ├── cli_parser_server.h
│   │   ├── cli_parser_server.cpp
│   │   ├── cli_parser_client.h
│   │   ├── cli_parser_client.cpp
│   │   ├── server_main.cpp
│   │   └── client_main.cpp
│   │
│   ├── desktop/
│   │   └── qt/
│   │
│   └── android/
│       └── AudioShare/
│
└── tests/
```

---

# 17. gRPC Proto

当前正式控制协议：

```proto
syntax = "proto3";

package aqua.pb;


service AudioService {

  // 创建 session
  rpc Connect(
      ConnectRequest
  )
  returns(
      ConnectResponse
  );

  // 保活
  rpc KeepAlive(
      KeepAliveRequest
  )
  returns(
      KeepAliveResponse
  );

  // 删除 session
  rpc Disconnect(
      DisconnectRequest
  )
  returns(
      Empty
  );
}


message Empty
{
}


message ConnectRequest
{
  // 可选，仅用于日志
  string client_name = 1;
}


message ConnectResponse
{
  // SessionManager 生成的 ID
  uint32 session_id = 1;

  // UDP Server endpoint
  UdpEndpoint udp = 2;

  // Server 固定 AudioFormat
  AudioFormat audio_format = 3;
}


message UdpEndpoint
{
  string address = 1;

  uint32 port = 2;
}


message KeepAliveRequest
{
  uint32 session_id = 1;
}


message KeepAliveResponse
{
  bool success = 1;
}


message DisconnectRequest
{
  uint32 session_id = 1;
}


message AudioFormat
{
  enum Encoding {

    ENCODING_INVALID = 0;

    ENCODING_PCM_S16LE = 1;

    ENCODING_PCM_S32LE = 2;

    ENCODING_PCM_F32LE = 3;

    ENCODING_PCM_S24LE = 4;

    ENCODING_PCM_U8 = 5;
  }

  Encoding encoding = 1;

  uint32 channels = 2;

  uint32 sample_rate = 3;
}
```

---

# 18. 开发路线

## Milestone 0：工程基础

- CMake
- C++20
- Core library
- spdlog
- GoogleTest
- Asio
- 基础 UDP Transport

---

## Milestone 1：Windows PCM

实现：

- WASAPI Loopback Capture
- PCM F32LE
- UDP Unicast
- WASAPI Playback

暂时不实现：

- gRPC
- NAT
- Jitter Buffer
- Clock Sync
- Opus

目标：

```text
Windows
    |
    | PCM / UDP
    v
Windows
```

完成最基本端到端音频链路。

---

## Milestone 2：SessionManager

实现：

- `uint32_t session_id`
- Session 创建
- Session 删除
- endpoint 保存
- `last_seen`
- timeout
- UDP HELLO
- UDP HELLO_ACK

目标：

```text
Client
   |
   | gRPC Connect
   v
Server
   |
   | session_id
   v
Client
   |
   | UDP HELLO
   v
Server
   |
   | HELLO_ACK
   v
Client
```

---

## Milestone 3：gRPC + NAT

实现：

- gRPC Server
- gRPC Client
- Connect
- KeepAlive
- Disconnect
- 固定 UDP media port
- NAT endpoint 自动发现
- SessionManager 与 UDP Gateway 集成

目标：

**Client 位于一层 NAT 后仍然可以连接公网 Server。**

---

## Milestone 4：基础稳定性

实现：

- sequence
- sample_position
- packet loss detection
- duplicate detection
- 基础 Jitter Buffer
- packet loss 静音填充

目标：

```text
正常网络：
低延迟

轻微丢包：
不崩溃
不产生严重爆音
```

---

## Milestone 5：Clock Sync

实现：

- Sender Report
- Receiver Report
- 网络延迟估计
- clock drift 估计
- resampler 微调

目标：

**长时间运行不会因为两个设备的声卡时钟差异而逐渐漂移。**

---

## Milestone 6：跨平台

实现：

- PipeWire
- AAudio
- Linux
- Android
- Qt6
- Kotlin + JNI

目标：

```text
Windows <-> Linux
Windows <-> Android
Linux   <-> Android
```

---

## Milestone 7：Opus

只有 PCM 链路、NAT、同步和 Jitter Buffer 稳定后，才加入：

- Opus
- Codec abstraction
- bitrate
- frame duration
- PLC
- FEC

Opus 不应影响当前 PCM 协议的基本架构。

---

# 19. 当前明确不做的事情

在没有明确需求之前，不要增加：

- UUID
- session token
- 用户系统
- 账号系统
- TLS 自定义认证
- STUN
- TURN
- ICE
- Codec negotiation
- Server-side resampling
- Server-side mixing
- Multicast
- Room
- Cloud service
- 复杂 RPC
- 每 Session 一个 UDP port

当前系统应该保持：

```text
gRPC
  |
  +-- Connect
  +-- KeepAlive
  +-- Disconnect

UDP
  |
  +-- HELLO
  +-- HELLO_ACK
  +-- AUDIO
```

---

# 20. 核心设计总结

Aqua 的第一版核心可以概括为：

```text
             ┌─────────────────┐
             │   gRPC Server   │
             │                 │
             │ Connect         │
             │ KeepAlive       │
             │ Disconnect      │
             └────────┬────────┘
                      │
                      v
              SessionManager
                      │
                      │ session_id
                      │ endpoint
                      v
             ┌─────────────────┐
             │   UDP Server    │
             │                 │
             │ HELLO           │
             │ HELLO_ACK       │
             │ AUDIO           │
             └────────┬────────┘
                      │
                      │ UDP
                      v
                    NAT
                      │
                      v
                   Client
```

核心原则：

> **gRPC 管连接，SessionManager 管状态，UDP 管音频，Audio Backend 管设备，Client 管自己的格式转换。**

Server 不参与音频格式转换，也不承担音频设备相关逻辑。

这是当前阶段最重要的架构边界。