# Aqua — 底层架构与接口设计

> 目标：在 Windows / Linux / Android 等主流平台之间，以足够低的延迟将一台设备的音频实时传输到另一台设备回放。
>
> 核心使用现代 C++23，CMake 构建；UI 与核心完全解耦。
>
> 当前第一阶段以 **PCM + UDP + gRPC + 一层 NAT 穿透** 为核心目标，优先验证低延迟音频链路和 NAT 环境下的可靠连接。

## 目录

| #  | 章节                           | 关注点                                |
|----|--------------------------------|---------------------------------------|
| 0  | 项目目标与当前原则             | 范围、阶段目标、不做的事              |
| 1  | 技术栈                         | 语言 / 构建 / 依赖                    |
| 2  | 分层架构                       | UI ↔ Core 边界、平台隔离              |
| 3  | Audio Format                   | Server 固定格式、原生结构、不转换原则 |
| 4  | gRPC Control Plane             | Connect / KeepAlive / Disconnect      |
| 5  | SessionManager                 | 状态机、线程安全、ID 生成             |
| 6  | NAT Traversal                  | 一层 NAT、HELLO 握手、单一 UDP 端口   |
| 7  | UDP Packet                     | 自定义二进制、AudioPacketHeader       |
| 8  | UDP Control Packet             | PacketType、HELLO / HELLO_ACK         |
| 9  | 音频数据流                     | Sender / Receiver 管线                |
| 10 | 线程模型                       | Audio / UDP / Control 线程约束        |
| 11 | RingBuffer                     | SPSC、音频回调隔离                    |
| 12 | Jitter Buffer                  | 排序、去重、静音填充                  |
| 13 | Clock Synchronization          | sample_position、不依赖 wall clock    |
| 14 | Client Audio Format Conversion | Client 自负转换                       |
| 15 | 低延迟原则                     | UDP、热路径约束                       |
| 16 | 目录结构                       | 当前实际 + 目标 + 约束                |
| 17 | gRPC Proto                     | 完整 proto 定义                       |
| 18 | 开发路线                       | Milestone 0 ~ 7                       |
| 19 | 当前明确不做的事情             | 负面清单                              |
| 20 | 核心设计总结                   | 架构边界一句话                        |
| 21 | 模块依赖图                     | target 依赖方向                       |
| 22 | 模块接口规范                   | 各模块公共 API 契约                   |
| 23 | C API 边界                     | UI ↔ Core 的 C ABI                    |
| 24 | 并发模型                       | 线程清单、io_context、关闭顺序        |
| 25 | 错误处理策略                   | 分层策略、断连恢复                    |
| 26 | 配置策略                       | CLI、超时、不引入配置文件             |
| 27 | 日志规范                       | 级别、必含字段                        |
| 28 | 构建系统                       | CMake target、preset、vcpkg           |
| 29 | 测试策略                       | 单元 / 集成范围与约束                 |
| 30 | 实现状态                       | 已完成 / 当前位置 / 下一步            |

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

当前阶段 **不实现**：

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

| 层级          | 技术            | 说明                                     |
|---------------|-----------------|------------------------------------------|
| 语言          | C++23           | 核心实现（CMake 强制 `CXX_STANDARD=23`） |
| 构建          | CMake ≥ 4.2     | 跨平台构建，配合 vcpkg manifest          |
| 包管理        | vcpkg           | manifest 模式，依赖锁定在 `vcpkg.json`   |
| 异步网络      | Asio（独立版）  | UDP 数据面                               |
| 控制面        | gRPC + protobuf | Session 控制                             |
| Windows 音频  | WASAPI          | 采集 / 播放                              |
| Linux 音频    | PipeWire        | 后续实现                                 |
| Android 音频  | AAudio          | 后续实现                                 |
| 桌面 UI       | Qt6             | Windows / Linux                          |
| Android UI    | Kotlin + JNI    | Android                                  |
| 缓冲          | SPSC RingBuffer | 音频线程之间传输                         |
| Jitter Buffer | 自定义          | 后续加入                                 |
| 日志          | spdlog          | 核心库日志                               |
| 命令行        | cxxopts         | server / client CLI 解析                 |
| 测试          | GoogleTest      | 单元测试                                 |
| Codec         | PCM             | 当前阶段唯一格式                         |
| Codec         | Opus            | 后续阶段                                 |

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

`AudioFormat` 只描述 **音频数据本身**。

不要在 AudioFormat 中加入：

```text
frame_samples
packet_size
jitter_buffer_size
target_latency
```

这些属于传输层或播放策略，而不是 AudioFormat。

---

## 3.4 原生 AudioFormat（C++ 侧）

proto 生成的 `aqua::pb::AudioFormat` 用于 gRPC 线缆协议。 音频管线内部（采集、RingBuffer、Jitter Buffer、播放） **不直接使用
proto 类型**， 而是使用 `src/core/public/audio_format.h` 中的原生结构：

```cpp
namespace aqua {

enum class AudioEncoding : std::uint8_t {
    Invalid  = 0,
    PcmS16LE = 1,
    PcmS32LE = 2,
    PcmF32LE = 3,
    PcmS24LE = 4,
    PcmU8    = 5,
};

struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::Invalid;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    bool valid() const noexcept;
    std::uint32_t bytes_per_sample() const noexcept;
    std::uint32_t frame_bytes() const noexcept; // bytes_per_sample * channels
};
}
```

约束：

- `AudioEncoding` 枚举值 **必须** 与 `aqua_service.proto` 中 `AudioFormat.Encoding` 数值一一对应。
- 修改任何一端都必须同步另一端，并在 PR 中明确说明。
- 在 gRPC 边界提供 `pb::AudioFormat <-> aqua::AudioFormat` 的双向转换函数（后续 M3 实现）。
- 原生结构只含 POD 字段，可自由拷贝，不持有资源。

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

    enum class SessionState : uint8_t {
        Created    = 0, // 已通过 gRPC Connect 创建，尚未收到 UDP HELLO
        Connecting = 1, // 保留状态，当前阶段不使用
        Connected  = 2, // UDP 握手完成，可收发音频
        Expired    = 3, // 超时未通信，等待回收
        Closed     = 4, // 已主动 Disconnect，等待回收
    };

    struct SessionInfo {

        session_id_t session_id;

        // UDP NAT 映射地址
        asio::ip::udp::endpoint endpoint;

        // 创建时间
        std::chrono::steady_clock::time_point created_at;

        // 最近一次 UDP 通信
        std::chrono::steady_clock::time_point last_seen;

        // 当前 Session 状态
        SessionState state = SessionState::Created;
    };

};
```

## 5.1 Session 状态机

```text
   gRPC Connect
        |
        v
    Created ──────gRPC Disconnect──────> Closed
        |                                  ^
        | UDP HELLO                        |
        v                                  |
    Connected ──────timeout──────────────> Expired
        |                                  ^
        | timeout                          |
        v                                  |
    (any state) ───────────────────────────┘
```

状态迁移规则：

```text
Created   + establish_udp()      -> Connected
Created   + timeout / disconnect -> Closed / Expired
Connected + touch_session()      -> Connected (刷新 last_seen)
Connected + timeout              -> Expired
任何状态  + remove_session()      -> (删除)
```

`is_connected()` 仅在 `state == Connected` 时返回 true。

`establish_udp()` 是状态从 `Created` 进入 `Connected` 的唯一入口， 同时记录 NAT 后的真实 endpoint 并刷新 `last_seen`。

## 5.2 SessionManager 不负责

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
    +-- state
```

## 5.3 线程安全

SessionManager 使用 `std::shared_mutex`：

- 读操作（`get_session` / `get_endpoint` / `is_connected` / `session_count` / `collect_expired_sessions`）持有共享锁
- 写操作（`create_session` / `remove_session` / `establish_udp` / `touch_session`）持有排他锁

调用方可在多线程并发访问，但 **禁止在持有 SessionInfo 引用期间回调 SessionManager**， 避免递归锁。正确做法：拷贝出
`SessionInfo` 后再释放锁。

## 5.4 Session ID 生成

```text
16 bit random instance_id (进程启动时随机)
+
16 bit 自增 counter
=
32 bit session_id
```

`instance_id` 在构造时由 `std::random_device` 生成，进程生命周期内不变。
`counter` 单调递增。同一进程内 ID 严格递增，跨进程靠 `instance_id` 区分。

ID 仅保证当前 Server 生命周期内尽量不冲突，不保证全局唯一，也不作为安全凭据。

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

## 16.1 当前实际结构

```text
aqua/
├── CMakeLists.txt              # 顶层构建脚本
├── CMakePresets.json           # 多平台 preset（windows/linux/macos）
├── vcpkg.json                  # 依赖清单（manifest 模式）
├── AGENT.md                    # 本文档（架构与接口设计的唯一权威）
├── .clang-format
│
├── proto/
│   └── aqua_service.proto      # gRPC 控制面协议定义
│
├── src/
│   ├── core/
│   │   ├── public/
│   │   │   └── audio_format.h  # 原生 AudioFormat（不依赖 proto）
│   │   ├── logger/
│   │   │   ├── logger.h
│   │   │   └── logger.cpp
│   │   └── session/
│   │       ├── session_manager.h
│   │       └── session_manager.cpp
│   └── main/
│       ├── cli_parser_server.h
│       ├── cli_parser_server.cpp
│       ├── cli_parser_client.h
│       ├── cli_parser_client.cpp
│       ├── server_main.cpp
│       └── client_main.cpp
│
└── tests/
    ├── CMakeLists.txt
    ├── test_log.cpp
    ├── test_session_manager.cpp
    ├── test_cli_parser_server.cpp
    └── test_cli_parser_client.cpp
```

## 16.2 目标结构（按 Milestone 渐进落地）

未实现的目录在对应 Milestone 开始时再创建， **不要预先建空目录**。

```text
src/
├── core/
│   ├── public/                 # [已建] 跨模块公共类型（AudioFormat 等）
│   ├── logger/                 # [已建] spdlog 封装
│   ├── session/                # [已建] SessionManager
│   ├── audio/
│   │   ├── backend/
│   │   │   ├── wasapi/         # [M1] Windows 采集 / 播放
│   │   │   ├── pipewire/       # [M6] Linux
│   │   │   └── aaudio/         # [M6] Android
│   │   └── ringbuffer/         # [M1] SPSC RingBuffer
│   ├── net/
│   │   ├── transport/          # [M1/M3] UDP socket 封装
│   │   ├── packet/             # [M1/M4] AudioPacket / ControlPacket 编解码
│   │   └── nat/                # [M3] HELLO / HELLO_ACK 握手
│   ├── grpc/                   # [M3] grpc_server / grpc_client
│   └── jitter_buffer/          # [M4] 基础 Jitter Buffer
├── main/                       # [已建] CLI 与可执行入口
├── desktop/
│   └── qt/                     # [M6] Qt6 UI
└── android/
    └── AudioShare/             # [M6] Kotlin + JNI
```

## 16.3 目录约束

- `src/core/public/` 下的头文件不得依赖 proto、Asio、平台音频 SDK。
- `src/core/audio/backend/` 下的平台代码不得被 core 其他模块直接 include， 必须通过 `audio_backend.h` 抽象接口暴露。
- `src/main/` 可以依赖 core + cxxopts，但不实现核心逻辑。
- `tests/` 镜像 `src/` 的模块布局，测试文件命名 `test_<module>.cpp`。

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

---

# 21. 模块依赖图

```text
                    ┌──────────────────────────────┐
                    │           main (exe)          │
                    │  cli_parser_*  server/client  │
                    └──────────────┬───────────────┘
                                   │
                                   v
                    ┌──────────────────────────────┐
                    │          aqua_core            │
                    │                              │
                    │  ┌──────────┐  ┌───────────┐ │
                    │  │  grpc    │  │   net     │ │
                    │  │  server/ │  │ transport │ │
                    │  │  client  │  │  packet   │ │
                    │  └────┬─────┘  │   nat     │ │
                    │       │        └─────┬─────┘ │
                    │       │              │       │
                    │       v              v       │
                    │  ┌──────────────────────────┐ │
                    │  │     session_manager      │ │
                    │  └──────────────────────────┘ │
                    │  ┌──────────┐  ┌───────────┐ │
                    │  │  audio   │  │  jitter   │ │
                    │  │ backend  │  │  buffer   │ │
                    │  │ringbuffer│  └───────────┘ │
                    │  └──────────┘                │
                    │  ┌──────────┐                │
                    │  │  logger  │  (横切，所有模块可用)│
                    │  └──────────┘                │
                    └──────────────┬───────────────┘
                                   │
                                   v
                    ┌──────────────────────────────┐
                    │          aqua_proto           │
                    │   pb::AudioService  pb::*     │
                    └──────────────┬───────────────┘
                                   │
                                   v
                    asio  spdlog  gRPC  protobuf  cxxopts
```

依赖方向规则：

- `main` → `aqua_core` → `aqua_proto`（单向，上层依赖下层）
- `logger` 是横切关注点，任何模块都可依赖，但 **logger 不得反向依赖任何业务模块**。
- `audio/backend` 只通过抽象接口被 `aqua_core` 使用，平台实现（wasapi/pipewire/aaudio）互不可见。
- `session_manager` 不依赖 `net` / `grpc` / `audio`，是纯状态容器。
- `tests` 可依赖所有上层 target。

---

# 22. 模块接口规范

本节定义各模块的公共接口契约。实现时不得超出此契约添加跨模块依赖。

## 22.1 logger

`src/core/logger/logger.h`

```cpp
namespace aqua {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void set_log_level(LogLevel level);

// 非格式化接口（string_view，无分配风险）
void log_trace(std::string_view message);
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

// 格式化接口（基于 spdlog::fmt），用于带变量的日志
template <typename... Args>
void log_info_fmt(spdlog::format_string_t<Args...> fmt, Args&&... args);
// ...其余级别同构
}
```

约束：

- 薄封装 spdlog default logger，线程安全由 spdlog 保证。
- 热路径（audio callback / UDP 收发）只允许 `log_trace` / `log_debug`， 且默认级别下应被过滤；禁止在热路径使用 `_fmt`
  排版复杂对象。

## 22.2 session_manager

`src/core/session/session_manager.h`（见 §5）

公共方法契约：

| 方法                                | 线程安全 | 失败行为                                               |
|-------------------------------------|----------|--------------------------------------------------------|
| `create_session()`                  | 排他锁   | 返回 `std::nullopt`（ID 空间耗尽，理论上不可能）       |
| `remove_session(id)`                | 排他锁   | 不存在返回 `false`，不抛异常                           |
| `get_session(id)`                   | 共享锁   | 不存在返回 `std::nullopt`                              |
| `get_endpoint(id)`                  | 共享锁   | 不存在或未握手返回 `std::nullopt`                      |
| `establish_udp(id, ep)`             | 排他锁   | 不存在返回 `false`；存在则覆盖 endpoint 并置 Connected |
| `touch_session(id)`                 | 排他锁   | 不存在返回 `false`                                     |
| `is_connected(id)`                  | 共享锁   | 不存在返回 `false`                                     |
| `collect_expired_sessions(timeout)` | 共享锁   | 返回超时 ID 列表，不自动删除                           |
| `session_count()`                   | 共享锁   | 永不失败                                               |

`collect_expired_sessions` **只读不删**，调用方拿到列表后自行 `remove_session`， 避免在持有共享锁时升级为排他锁。

## 22.3 audio_format

`src/core/public/audio_format.h`（见 §3.4）

POD 类型，无依赖，全 `noexcept` 接口。

## 22.4 net/transport（M1/M3 待实现）

UDP socket 封装，基于 `asio::io_context`。

```cpp
namespace aqua::net {

class UdpTransport {
public:
    using ReceiveHandler = std::function<void(
        const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data)>;

    UdpTransport(asio::io_context& ioc);
    bool bind(const asio::ip::udp::endpoint& local);
    void start_receive(ReceiveHandler handler);
    void send(const asio::ip::udp::endpoint& target,
              std::span<const std::byte> data);
    void stop();
};
}
```

约束：

- 不持有 SessionManager 引用；收到包后通过回调上交，由上层做路由。
- 接收缓冲预分配固定大小（65536 字节，覆盖最大 UDP datagram），**不在回调中分配堆内存**。
- 回调在 io_context 线程执行，禁止阻塞；重活投递到其他线程。

## 22.5 net/packet（M1/M4 待实现）

无状态编解码，纯函数式：

```cpp
namespace aqua::net {

enum class PacketType : std::uint8_t {
    Hello    = 1,
    HelloAck = 2,
    Audio    = 3,
};

struct AudioPacketHeader {
    std::uint32_t session_id;
    std::uint32_t sequence;
    std::uint32_t sample_position;
    std::uint16_t payload_size;
};

// 序列化 / 反序列化返回 std::span 或 optional，不抛异常。
std::span<const std::byte> encode_hello(std::uint32_t session_id, Buffer& out);
std::optional<HelloPacket>  decode_hello(std::span<const std::byte> in);
// ... Audio 同构
}
```

约束：

- 所有整数按 **小端序** 读写（与 PCM 编码一致）。
- 解码失败返回 `std::nullopt`，由调用方丢弃包。
- 不做长度校验以外的语义校验（session_id 是否存在由上层判断）。

## 22.6 grpc（M3 待实现）

`src/core/grpc/grpc_server.h` / `grpc_client.h`

```cpp
namespace aqua::grpc {

class AudioServiceImpl final : public pb::AudioService::Service {
public:
    explicit AudioServiceImpl(SessionManager& sessions, AudioFormat server_format);
    grpc::Status Connect(...) override;
    grpc::Status KeepAlive(...) override;
    grpc::Status Disconnect(...) override;
};

class GrpcServer {
public:
    GrpcServer(SessionManager& sessions, AudioFormat format,
               std::string bind_ip, std::uint16_t port);
    void run();   // 阻塞
    void shutdown();
};
}
```

约束：

- gRPC 服务持有 `SessionManager` 引用， **不拥有**它（生命周期由 main 管理）。
- `Connect` 内部调用 `sessions.create_session()`，把返回的 ID 与 UDP endpoint、AudioFormat 写入响应。
- 不在 gRPC 线程做网络 I/O 之外的工作。

## 22.7 audio/backend（M1 待实现）

抽象接口在 `src/core/audio/backend/audio_backend.h`，平台实现在子目录：

```cpp
namespace aqua::audio {

class CaptureBackend {
public:
    using CaptureCallback = std::function<void(std::span<const std::byte> pcm)>;
    virtual ~CaptureBackend() = default;
    virtual bool start(AudioFormat format, CaptureCallback cb) = 0;
    virtual void stop() = 0;
};

class PlaybackBackend {
public:
    virtual ~PlaybackBackend() = default;
    virtual bool start(AudioFormat format) = 0;
    virtual void submit(std::span<const std::byte> pcm) = 0;
    virtual void stop() = 0;
};

// 工厂：平台相关，根据编译期宏选择 wasapi/pipewire/aaudio
std::unique_ptr<CaptureBackend> create_capture_backend();
std::unique_ptr<PlaybackBackend> create_playback_backend();
}
```

约束：

- 平台实现不得泄漏到接口（头文件不 include `<windows.h>` / `<mmdeviceapi.h>` 等）。
- 回调在音频实时线程触发，遵守 §10 / §15.2 约束（无锁、无分配、无阻塞）。
- 回调内只做 RingBuffer 写入， **不直接调用 UDP / SessionManager**。

## 22.8 ringbuffer（M1 待实现）

`src/core/audio/ringbuffer/spsc_ringbuffer.h`

```cpp
namespace aqua::audio {

// 单生产者单消费者无锁环形缓冲
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(std::size_t capacity_bytes);
    std::size_t write(std::span<const std::byte> data) noexcept; // 返回实际写入字节数
    std::size_t read(std::span<std::byte> out) noexcept;         // 返回实际读出字节数
    std::size_t available_read() const noexcept;
    std::size_t available_write() const noexcept;
};
}
```

约束：

- 容量必须为 2 的幂，便于掩码取模。
- 只允许 1 写 1 读；多生产者/消费者场景需外层串行化。
- 写满返回实际写入量（不阻塞、不覆盖未读数据），调用方负责丢弃或统计。

## 22.9 jitter_buffer（M4 待实现）

`src/core/jitter_buffer/jitter_buffer.h`

```cpp
namespace aqua::jitter {

class JitterBuffer {
public:
    explicit JitterBuffer(AudioFormat format, std::size_t target_latency_frames);
    void push(const AudioPacketHeader& hdr, std::span<const std::byte> pcm);
    // 返回按 sequence 排序后的 PCM；缺包填充静音
    std::size_t pop(std::span<std::byte> out, std::size_t frames);
    void reset();
};
}
```

约束：

- 内部按 `sequence` 排序，检测丢包并填零。
- `target_latency` 由配置决定，不属于 AudioFormat。
- 不做重传（UDP 语义）。

---

# 23. C API 边界（UI ↔ Core）

桌面 UI（Qt6）与 Android UI（Kotlin/JNI）通过 **C ABI** 调用核心库， 确保符号稳定、跨编译器兼容。

## 23.1 头文件

`include/aqua.h`（待创建）：

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef struct aqua_client_t aqua_client_t;
typedef struct aqua_server_t aqua_server_t;

typedef struct {
    int      encoding;     // aqua::AudioEncoding 数值
    uint32_t channels;
    uint32_t sample_rate;
} aqua_audio_format_t;

// Client
aqua_client_t* aqua_client_create(void);
void           aqua_client_destroy(aqua_client_t* c);
int            aqua_client_connect(aqua_client_t* c,
                                   const char* server_ip,
                                   uint16_t    rpc_port,
                                   aqua_audio_format_t* out_format);
int            aqua_client_disconnect(aqua_client_t* c);
void           aqua_client_set_log_level(int level);

// Server
aqua_server_t* aqua_server_create(void);
void           aqua_server_destroy(aqua_server_t* s);
int            aqua_server_run(aqua_server_t* s,
                               const char* bind_ip,
                               uint16_t    rpc_port,
                               uint16_t    udp_port);
void           aqua_server_shutdown(aqua_server_t* s);

#ifdef __cplusplus
}
#endif
```

## 23.2 边界规则

- C API 只暴露句柄（`aqua_client_t*`）， **不暴露任何 C++ 类布局**。
- 所有 C 函数返回 `int` 状态码：`0 = OK`，`<0 = 错误`；详细错误走日志。
- 跨边界不抛 C++ 异常，内部用 `try/catch` 全捕获并转成负返回码。
- 跨边界不传递 `std::string`，统一用 `const char*`（UTF-8）+ 调用方释放。
- 字符串所有权：入参由调用方拥有；出参指针在句柄销毁前有效。
- C++ 侧用 pImpl 隔离实现，头文件不含 STL / gRPC / Asio 类型。

---

# 24. 并发模型

## 24.1 线程清单

| 线程           | 所属          | 职责                                   | 阻塞约束        |
|----------------|---------------|----------------------------------------|-----------------|
| Main           | main          | CLI 解析、构造对象、启动、等待退出信号 | 可阻塞          |
| gRPC Server    | grpc          | CompletionQueue / 同步 RPC 服务        | 可阻塞          |
| gRPC Client    | grpc (client) | 异步 RPC 等待                          | 可阻塞          |
| UDP I/O        | asio          | `io_context.run()`，收发 UDP           | 不可阻塞        |
| Audio Capture  | 平台音频      | WASAPI/PipeWire/AAudio 回调            | 实时约束（§10） |
| Audio Playback | 平台音频      | WASAPI/PipeWire/AAudio 回调            | 实时约束（§10） |
| Jitter Worker  | jitter        | 排序、静音填充（可选，M4）             | 可阻塞          |

## 24.2 io_context 策略

- UDP 数据面使用 **单个** `asio::io_context`，可由 1~N 线程 `run()`。
- 第一阶段建议单线程 `run()`，避免包乱序与锁竞争。
- io_context 不与 gRPC 共享线程池。

## 24.3 跨线程通信路径

```text
Audio Capture Thread ──SPSC RingBuffer──> UDP I/O Thread ──UDP──> 远端
远端 ──UDP──> UDP I/O Thread ──SPSC RingBuffer / Jitter──> Audio Playback Thread
gRPC Thread ──SessionManager(shared_mutex)──> UDP I/O Thread (查 endpoint)
```

允许的跨线程共享：

- `SessionManager`（通过 `shared_mutex`）
- `SpscRingBuffer`（无锁，单写单读）
- `std::atomic` 标志位（如 `running_`）

禁止的跨线程共享：

- 直接共享 `asio::ip::udp::socket`（必须经 `io_context.post()` 调度）
- 在音频回调线程访问 SessionManager（锁会阻塞实时线程）
- 在 UDP 回调中直接调用阻塞 gRPC

## 24.4 退出与关闭顺序

```text
1. main 收到 SIGINT/退出信号，置 running_ = false
2. grpc_server->shutdown()
3. udp_transport->stop()
4. io_context->stop()
5. audio_backend->stop()
6. join 所有线程
7. 析构对象（逆序：client/server → core → proto）
```

---

# 25. 错误处理策略

## 25.1 分层策略

| 层             | 策略                                            |
|----------------|-------------------------------------------------|
| 音频回调       | **不抛异常**；失败静默丢弃或填零，记 trace 日志 |
| UDP 收发       | **不抛异常**；解码失败丢包，send 失败记 warn    |
| net/packet     | 返回 `std::optional`，不抛异常                  |
| SessionManager | 返回 `bool` / `optional`，不抛异常              |
| gRPC           | 用 `grpc::Status` 返回错误，不抛异常            |
| C API 边界     | `try/catch` 全捕获，返回负码                    |
| main           | 解析失败打印 stderr 并返回非零退出码            |

## 25.2 不可恢复错误

仅以下场景允许终止进程：

- proto 文件与原生 `AudioEncoding` 数值不一致（启动时 assert）
- io_context 启动失败
- 致命系统调用失败（bind 端口被占且无法重试）

其余错误一律降级处理（丢包、断开 session、返回错误码）， **不 crash**。

## 25.3 客户端断连恢复

- UDP 超时（默认 10s 未收包）→ 标记 session Expired，停止播放并填零。
- gRPC KeepAlive 失败 → 客户端重连（指数退避，上限 30s）。
- 重连后重新 `Connect`，获取新 session_id，重新 UDP 握手。

---

# 26. 配置策略

## 26.1 Server 配置（CLI）

```
aqua_server
  --bind-ip <ip>        默认 0.0.0.0
  --rpc-port <port>     默认 50051
  --udp-port <port>     默认 50000
  --help
  --version
```

## 26.2 Client 配置（CLI）

```
aqua_client
  --server-ip <ip>          默认 127.0.0.1
  --server-rpc-port <port>  默认 50051
  --help
  --version
```

## 26.3 Server AudioFormat 配置

第一阶段： **编译期固定**为 `PcmS16LE / 48000 / 2ch`（WASAPI Loopback 常见格式）。 后续可加 `--audio-format` CLI 参数，但运行期不可变。

## 26.4 超时参数

| 参数                | 默认 | 说明                            |
|---------------------|------|---------------------------------|
| UDP session timeout | 10 s | `collect_expired_sessions` 阈值 |
| KeepAlive 间隔      | 5 s  | Client 发送频率                 |
| Expired 清理周期    | 2 s  | Server 扫描周期                 |

这些常量集中在 `src/core/public/config.h`（待建），不散落各处。

## 26.5 不引入配置文件

第一阶段所有配置走 CLI + 编译期常量。 **不引入** YAML / JSON / TOML 配置文件，避免增加解析依赖与路径搜索复杂度。

---

# 27. 日志规范

## 27.1 级别使用

| 级别  | 使用场景                           |
|-------|------------------------------------|
| Trace | UDP 逐包、音频回调逐帧（默认关闭） |
| Debug | 状态机迁移、endpoint 变更          |
| Info  | 服务启动 / 停止、新 session 建立   |
| Warn  | 丢包、解码失败、端口重试           |
| Error | gRPC 失败、bind 失败、设备打开失败 |

## 27.2 必含字段

每条日志应能定位：

- `session_id`（若有）
- `endpoint`（若有）
- `sequence`（音频包相关）

格式示例：

```text
[info] session 7A31-0001 established UDP 203.0.113.10:54321
[warn] session 7A31-0001 seq 103 lost, filling silence
```

## 27.3 默认级别

- Server: `Info`
- Client: `Info`
- 调试时通过 CLI（后续加 `--log-level`）或环境变量切换。

---

# 28. 构建系统

## 28.1 CMake 目标

| Target        | 类型   | 说明                                                     |
|---------------|--------|----------------------------------------------------------|
| `aqua_proto`  | STATIC | proto 生成的 `*.pb.cc` / `*.grpc.pb.cc`                  |
| `aqua_core`   | STATIC | 核心库（logger / session / audio / net / grpc / jitter） |
| `aqua_server` | EXE    | Server 入口，链接 `aqua_core` + `cxxopts`                |
| `aqua_client` | EXE    | Client 入口，链接 `aqua_core` + `cxxopts`                |
| `aqua_tests`  | EXE    | GoogleTest，链接 `aqua_core` + `aqua_proto`              |

## 28.2 构建命令

```bash
# 配置（Windows x64 Debug）
cmake --preset windows-x64-debug -S D:\coding\aqua -B D:\coding\aqua\cmake_build\windows-x64-debug

# 编译
cmake --build D:\coding\aqua\cmake_build\windows-x64-debug --config Debug

# 测试
ctest --test-dir D:\coding\aqua\cmake_build\windows-x64-debug --build-config Debug --output-on-failure
```

## 28.3 Presets

`CMakePresets.json` 提供：

- `windows-x64-debug` / `windows-x64-release`（VS 生成器，x64-windows triplet）
- `linux-x64-debug` / `linux-x64-release`（Ninja Multi-Config，x64-linux）
- `macos-arm64-debug` / `macos-arm64-release`（Ninja Multi-Config，arm64-osx）

所有 preset 继承 `base`：`CXX_STANDARD=23`、`BUILD_TESTS=ON`、
`VCPKG_INSTALLED_DIR=${sourceDir}/vcpkg_installed`、导出 `compile_commands.json`。

## 28.4 依赖管理

- 全部依赖经 vcpkg manifest（`vcpkg.json`）安装，版本由 builtin-baseline 锁定。
- `vcpkg_installed/` 已加入 `.gitignore`， **不要提交**，也不要在清理构建时删除（重编译耗时长）。
- 当前依赖：`spdlog`、`cxxopts`、`grpc`、`asio`、`gtest`。

## 28.5 Windows 平台宏

`aqua_core` 公共定义：

```text
_UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
```

链接：`ole32`、`winmm`、`ws2_32`（后续 WASAPI 还需 `ole32` 已包含）。

---

# 29. 测试策略

## 29.1 单元测试范围

| 模块            | 测试重点                               |
|-----------------|----------------------------------------|
| logger          | 级别切换、格式化不抛异常               |
| session_manager | 创建/删除/握手/超时/计数/状态迁移      |
| cli_parser      | 默认值、自定义、help/version、非法端口 |
| audio_format    | bytes_per_sample / frame_bytes 各编码  |
| net/packet      | 编解码对称性、截断包返回 nullopt       |
| ringbuffer      | 读写指针、写满不覆盖、空读             |
| jitter_buffer   | 乱序排序、丢包静音、重复包             |

## 29.2 测试约束

- 测试不得依赖网络与音频硬件。
- 涉及 asio endpoint 的测试用 `127.0.0.1` + 随机高端口。
- 超时测试用真实 `sleep_for`（已有 `CollectExpiredSessions` 用例）， 时长保持 < 2s，避免拖慢 CI。
- 测试文件命名 `test_<module>.cpp`，镜像 `src/` 布局。

## 29.3 集成测试（后续）

M3 完成后增加端到端测试：

- 同进程内启动 mock gRPC server + UDP transport，验证 Connect → HELLO → AUDIO 流程。
- 不做跨进程测试，避免 CI 复杂度。

---

# 30. 实现状态

> 本节追踪当前实现进度，与 §18 Milestone 对应。每次合入需更新。

## 30.1 已完成

### Milestone 0：工程基础

- ✅ **CMake + vcpkg manifest + presets**（win/linux/macos）
- ✅ **C++23** 标准强制
- ✅ **logger**：spdlog 薄封装，5 级日志 + 格式化接口
- ✅ **session_manager**：create / remove / get / establish_udp / touch / is_connected / collect_expired / count
- ✅ **SessionState 状态机**：Created / Connecting / Connected / Expired / Closed
- ✅ **audio_format**：原生 `AudioFormat` + `AudioEncoding`，与 proto 同步
- ✅ **proto**：`AudioService`（Connect / KeepAlive / Disconnect）+ `AudioFormat` + `UdpEndpoint`
- ✅ **CLI**：`cli_parser_server`（--bind-ip / --rpc-port / --udp-port）/ `cli_parser_client`（--server-ip / --server-rpc-port / --server-udp-port）
- ✅ **UDP Transport**：Asio 封装，异步收发，预分配接收缓冲，回环测试通过
- ✅ **SPSC RingBuffer**：无锁环形缓冲，容量取整 2 的幂，并发读写测试通过

### Milestone 1：Windows PCM

- ✅ **Packet codec**：Hello / HelloAck / Audio 二进制编解码，小端序，零拷贝解码 payload
- ✅ **WASAPI Loopback 采集**：COM RAII，自动 mix format 探测，polling 模式
- ✅ **WASAPI 播放**：共享模式渲染，FillCallback 回调填充 + 静音填充
- ✅ **Audio Backend 抽象**：`CaptureBackend` / `PlaybackBackend` 接口 + 工厂函数
- ✅ **Server 端到端**：WASAPI 采集 → RingBuffer → Packetizer（10ms/包）→ UDP 发送
- ✅ **Client 端到端**：HELLO → UDP 接收 → depacketize → RingBuffer → WASAPI 播放
- ✅ **单元测试**：logger / session_manager / cli_parser / audio_format / ringbuffer / packet / udp_transport（54 用例全通过）

## 30.2 当前位置

**Milestone 0 + Milestone 1 已完成。**

端到端 PCM 音频链路可用：
```text
Windows (WASAPI Loopback) → PCM/UDP → Windows (WASAPI Playback)
```

运行方式：
```bash
# Server（在一台 Windows 上）
aqua_server --udp-port 50000

# Client（在另一台或同一台 Windows 上）
aqua_client --server-ip <server_ip> --server-udp-port 50000
```

## 30.3 下一步优先级（Milestone 2 → 3）

1. **M2 SessionManager 集成**：将 SessionManager 接入 UDP HELLO/HELLO_ACK 流程，
   替换 M1 的 "最后发 HELLO 的客户端" 简化逻辑
2. **M3 gRPC + NAT**：实现 gRPC Server/Client（Connect/KeepAlive/Disconnect），
   通过 Connect 返回 session_id + UDP endpoint + AudioFormat
3. **M4 基础稳定性**：sequence 丢包检测、Jitter Buffer、静音填充

## 30.4 已知偏差与遗留

- **M1 无 session 管理**：server 使用 "最后发 HELLO 的客户端 endpoint" 作为发送目标，
  session_id 固定为 0。M2 将接入真正的 SessionManager。
- **M1 无格式协商**：client 硬编码 F32LE/48k/2ch 播放格式（Windows 标准 mix format），
  若 server 设备 mix format 不同则可能失真。M3 通过 gRPC Connect 返回实际格式解决。
- **M1 无 Jitter Buffer**：client 直接用 RingBuffer 缓冲，无排序/去重/丢包检测。M4 加入。
- proto `AudioFormat.Encoding` 曾存在 `S32LE=3 / F32LE=2` 的值互换 bug，已修正。
- `src/core/public/` 目前只有 `audio_format.h`，`config.h`（超时常量）待 M3 时加入。
- `include/aqua.h`（C API）尚未创建，待 M6 引入 UI 时再建。