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
| 4  | gRPC Control Plane             | Connect / Disconnect                  |
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
- Disconnect

UDP：

- NAT 探测
- NAT 映射
- 音频数据
- HELLO 保活（刷新 NAT 映射 + server session last_seen）
- 后续序列号 / 同步信息

**gRPC 不承载音频数据，也不参与保活。** 保活完全由 UDP HELLO 完成（见 §6.3）。

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
Disconnect
```

不提供：

```text
GetAudioFormat
SetAudioFormat
KeepAlive
Codec negotiation
RegisterMediaEndpoint
```

不提供 KeepAlive 的原因：

- 保活完全由 UDP HELLO 承担：server 收到 HELLO 后 `establish_udp` → `touch_session`，刷新 `last_seen`（幂等）。
- UDP HELLO 同时刷新 NAT 映射（必须由 UDP 上行流量完成，gRPC 的 TCP 做不到）与 server session 状态，单路保活即可覆盖两者。
- gRPC 保活会在 UDP 全丢但 TCP 通时产生 "session 活但 NAT 映射死" 的隐蔽状态，删掉它简化了语义。

不提供 GetAudioFormat 的原因：

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
        Connecting = 1, // 保留状态，当前阶段未使用
        Connected  = 2, // UDP 握手完成，可收发音频
        Expired    = 3, // 保留状态，当前阶段未使用（超时直接 remove）
        Closed     = 4, // 保留状态，当前阶段未使用（Disconnect 直接 remove）
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

当前阶段实际使用的状态转换简化为两态（`Created` / `Connected`），超时与 Disconnect 均直接 `remove_session()` 删除，不经过
`Expired` / `Closed` 中间态：

```text
   gRPC Connect
        |
        v
    Created ──────establish_udp()────────> Connected
        |                                     |
        | remove_session()                    | remove_session()
        | (timeout / Disconnect)              | (timeout / Disconnect)
        v                                     v
      (删除)                                (删除)
```

状态迁移规则：

```text
Created   + establish_udp()      -> Connected
Created   + timeout / disconnect -> (remove)
Connected + establish_udp()      -> Connected (幂等，更新 endpoint + last_seen)
Connected + touch_session()      -> Connected (仅刷新 last_seen)
Connected + timeout              -> (remove)
任何状态  + remove_session()      -> (删除)
```

`is_connected()` 仅在 `state == Connected` 时返回 true。

`establish_udp()` 是状态从 `Created` 进入 `Connected` 的唯一入口， 同时记录 NAT 后的真实 endpoint 并刷新 `last_seen`。对已
`Connected` 的 session 调用 `establish_udp()` 是幂等的，用于 NAT remap 时更新 endpoint。

`for_each_connected(callback)` 遍历所有 `Connected` 状态的 session，callback 接收
`(session_id, endpoint)`，返回 `false` 停止遍历。在持有共享锁期间调用， **禁止在回调中 回调 SessionManager**（避免递归锁）。

`clear()` 清空所有 session，用于 server 优雅退出，返回被清理的 session 数量。

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

- 读操作（`get_session` / `get_endpoint` / `is_connected` / `session_count` / `collect_expired_sessions` /
  `for_each_connected`）持有共享锁
- 写操作（`create_session` / `remove_session` / `establish_udp` / `touch_session` / `clear`）持有排他锁

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

## 6.5 UDP HELLO 保活（单路保活）

保活完全由 UDP HELLO 承担，gRPC 不参与保活。HELLO 兼任两种角色：

1. **首次握手**：Created → Connected，记录 NAT 后的真实 endpoint。
2. **周期保活**：Client 按 `KEEPALIVE_INTERVAL`（1s）重发 HELLO，server 收到后：
    - `establish_udp()`（幂等，更新 endpoint 以应对 NAT remap）
    - `touch_session()`（刷新 `last_seen`，防止 session 超时）
    - 回复 HELLO_ACK（确认链路存活）

```text
Client ---UDP HELLO (每 1s)---> Server
                                  |
                                  +-- establish_udp (刷新 endpoint)
                                  +-- touch_session (刷新 last_seen)
                                  +-- HELLO_ACK (回复)
```

**为什么不用 gRPC KeepAlive**：

- NAT 映射只能由 UDP 上行流量刷新，gRPC（TCP）做不到 —— UDP HELLO 不可省。
- server 收到 HELLO 已经 `touch_session`，gRPC KeepAlive 在 session 保活上是冗余的。
- 双路保活会在 UDP 全丢但 TCP 通时产生 "session 活但 NAT 映射死" 的隐蔽状态，单路更简单。

**参数关系**（见 §26.4）：

```text
UDP_SESSION_TIMEOUT = 5s
KEEPALIVE_INTERVAL  = 1s   (5 次保活机会，容忍连续 4 次丢包)
```

server 仅在 HELLO 包上 `touch_session`， **不在 Audio 包上更新 last_seen** —— 否则恶意 client 持续发 Audio 包会让它的
session 永不过期。

---

## 6.6 NAT endpoint 不由 gRPC 提供

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
- Disconnect

控制面和 UDP 数据面完全隔离。保活不经过控制面（见 §6.3 UDP HELLO 保活）。

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

JitterBuffer 是 M4 的核心组件，负责在固定播放延迟下正确处理 UDP 的乱序、重复、丢包和 late packet。

## 12.1 核心原则

- **push 时不判定丢包**：收到包只做归类（future / expected / duplicate），不因 sequence gap 立即判定 lost
- **playout deadline 判定丢包**：每个包有自己的播放时刻，到 deadline 仍不在 buffer 中才判定 lost 并静音填充
- **late packet**：超过 deadline 后到达的包（`diff < 0`），记为 late 并丢弃
- **不依赖 timer**：JitterBuffer 只暴露 `next_playout_deadline()`，由外部调度器（steady_timer）驱动
- **不依赖固定 packet duration**：`packet_duration_` 由 `frames_per_packet` 和 `sample_rate` 推导，不硬编码 10ms

## 12.2 数据流

```text
UDP recv → decode_audio → JitterBuffer.push(seq, sample_pos, payload)
                                    │
                                    │ playout deadline
                                    ▼
                              pop_next(out)
                                    │
                                    ▼
                          SPSC RingBuffer.write(pcm)
                                    │
                                    ▼
                          WASAPI callback → RingBuffer.read(out)
```

JitterBuffer 不直接知道 RingBuffer 或 WASAPI，只负责按播放时间线输出连续 PCM。

## 12.3 播放时间线（Timeline）启动机制

JitterBuffer **不使用"收到 N 个包才开始播放"的计数式启动**。启动完全基于时间线：

1. 收到第一个包时，记录 `first_packet_time_ = clock::now()`
2. 设置 `next_deadline_ = first_packet_time_ + packet_duration_ * target_latency_packets_`
3. `next_playout_deadline()` 立即返回该 deadline（不需要等待 N 个包）
4. 外部调度器在 deadline 到达后调用 `pop_next()`
5. 每次 `pop_next()` 后，`next_deadline_ += packet_duration_`

这意味着 `target_latency = 3 包` 表示"第一个包到达 30ms 后开始播放"，而不是"收到 3 个包后开始播放"。

**为什么不用计数式启动**：如果收到 `100, 103, 105` 三个包，计数式启动会立即开始播放，但 `101, 102, 104` 都缺失，导致大量静音。时间线启动给这些包留出等待窗口。

## 12.4 调度器启动时机

JitterBuffer 调度器（`steady_timer` 驱动 `pop_next → ringbuffer.write`）**必须在 HELLO_ACK 后立即启动**，不能等待 WASAPI 初始化。

原因：服务器在收到 HELLO 后立即开始广播音频，WASAPI 初始化可能耗时数秒。如果此时 JitterBuffer 只 push 不 pop，sequence 会快速超过 capacity 导致持续 reset。

RingBuffer 128KB 容量足以缓冲 WASAPI 初始化期间 JitterBuffer 的输出（~340ms@48kHz/F32LE/2ch）。WASAPI 准备好后从 RingBuffer 消费。

## 12.5 内存布局

预分配连续 storage，不使用每 Slot 一个 vector：

```text
slots_:   [Slot][Slot][Slot][Slot]...   (metadata: sequence + valid)
storage_: [PCM_0][PCM_1][PCM_2]...      (连续内存，按 slot_index * payload_size 分区)
```

slot 空闲标记用 `bool valid`，不用 `sequence == 0`（因为 sequence=0 是合法值）。

slot index = `sequence & slot_mask_`（capacity 为 2 的幂）。
capacity = `std::bit_ceil(target_latency_packets * 2)`，给乱序留余量。
构造函数验证 capacity 必须为 2 的幂，否则抛 `std::invalid_argument`。

热路径零 heap allocation。

## 12.6 sequence 回绕处理

使用有符号差值比较：

```cpp
int32_t diff = static_cast<int32_t>(seq - next_pop_seq_);
// diff < 0: duplicate/late
// diff == 0: expected
// diff > 0: future (or gap)
// diff >= capacity: sequence jump too far → reset timeline
```

uint32_t 在 48kHz/10ms 下约 497 天回绕，差值法正确处理。

## 12.7 reset 语义

`reset()` 只清除**播放状态**（slot metadata + timeline），不清除：

- `storage_` 中的 PCM 数据（旧数据不会被读取因为 `valid = false`，避免大 memset）
- 统计计数器（`packets_received_` / `packets_lost_` / `duplicates_` / `late_packets_`）

统计在 session 生命周期内累积。sequence jump 触发的 reset 不丢失历史诊断数据。

## 12.8 静音填充

M4 使用 zero-fill。后续 M7（Opus）再考虑 PLC / FEC。

## 12.9 与 RingBuffer 的职责边界

| JitterBuffer | RingBuffer |
|---|---|
| packet 时间顺序 | PCM 字节流跨线程传递 |
| sequence reorder | producer/consumer 解耦 |
| duplicate detection | 不感知 packet / sequence |
| late packet detection | — |
| loss detection | — |
| playout deadline | — |
| packet concealment | — |
| occupancy statistics | — |

RingBuffer 不参与 packet/sequence 语义。

---

# 13. Clock Synchronization

**当前阶段（M4/M5）不做 clock synchronization，只做 clock drift 诊断。**

音频同步不能依赖 wall clock。

不使用：

```text
NTP
```

音频时间轴以 sample 为基础：

```text
sample_position
+
steady_clock
```

## 13.1 M5：playback-rate drift 诊断（只测不修）

不使用 HELLO 携带时间戳，而是基于 **RingBuffer 水位长期趋势** 检测 drift：

```text
RingBuffer occupancy
    ↓
short-term slope (~5s) + long-term slope (~60s)
    ↓
estimated playback-rate drift (ppm)
```

短期波动反映网络 jitter；长期趋势反映真实 clock drift。

命名使用 **estimated playback-rate drift**（而非 "clock drift"），因为测量的是 RingBuffer occupancy 变化趋势推断的 producer/consumer 速率差，不是直接测量两个物理晶振。

RTT 是 network diagnostic，**不用于** clock drift 估算。不设计 client_ts / server_ts 交叉比较。

Windows 平台可额外利用 `IAudioClock::GetPosition()` + `GetFrequency()` 直接测量 WASAPI device clock rate，比 RingBuffer slope 更直接（仅诊断）。

## 13.2 未来 correction（M5+）

不预设方案，根据 M5 实测数据决定：

- 小粒度 sample slip
- 极低比例 time-scale modification
- 平台特定时钟策略
- 无需主动 correction

**不使用 resampler。** resampler 增加复杂度和 CPU 开销，与低延迟目标矛盾，且后续 Opus codec 自带 PLC/FEC 可在 codec 层面处理。

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
│   └── aqua_service.proto      # gRPC 控制面协议定义（Connect / Disconnect，无 KeepAlive）
│
├── src/
│   ├── core/
│   │   ├── public/
│   │   │   ├── audio_format.h  # 原生 AudioFormat（不依赖 proto）
│   │   │   └── config.h        # 集中超时 / 保活常量
│   │   ├── logger/
│   │   │   ├── logger.h
│   │   │   └── logger.cpp
│   │   ├── session/
│   │   │   ├── session_manager.h
│   │   │   └── session_manager.cpp
│   │   ├── audio/
│   │   │   ├── backend/
│   │   │   │   ├── audio_backend.h          # CaptureBackend / PlaybackBackend 抽象接口
│   │   │   │   ├── audio_backend_factory.cpp
│   │   │   │   └── wasapi/                  # Windows WASAPI 采集 / 播放
│   │   │   │       ├── wasapi_common.h      # ComPtr + WAVEFORMATEX 互转（capture/playback 共用）
│   │   │   │       ├── wasapi_capture.{h,cpp}
│   │   │   │       └── wasapi_playback.{h,cpp}
│   │   │   └── ringbuffer/
│   │   │       └── spsc_ringbuffer.{h,cpp}
│   │   ├── jitter_buffer/
│   │   │   └── jitter_buffer.{h,cpp}        # [M4] JitterBuffer（playout deadline）
│   │   ├── diagnostics/
│   │   │   └── diagnostics_manager.{h,cpp}  # [M5] 诊断数据采集与输出
│   │   ├── net/
│   │   │   ├── transport/
│   │   │   │   └── udp_transport.{h,cpp}    # Asio UDP 封装
│   │   │   └── packet/
│   │   │       └── packet.{h,cpp}           # Hello / HelloAck / Audio 编解码
│   │   └── grpc/
│   │       ├── grpc_server.{h,cpp}          # AudioServiceImpl + GrpcServer（含 is_running()）
│   │       ├── grpc_client.{h,cpp}          # GrpcClient
│   │       └── audio_format_converter.{h,cpp}
│   └── main/
│       ├── cli_parser_common.h              # parse_port 共用工具
│       ├── cli_parser_server.{h,cpp}
│       ├── cli_parser_client.{h,cpp}
│       ├── server_main.cpp
│       └── client_main.cpp
│
└── tests/
    ├── CMakeLists.txt
    ├── test_log.cpp
    ├── test_session_manager.cpp
    ├── test_cli_parser_server.cpp
    ├── test_cli_parser_client.cpp
    ├── test_audio_format.cpp
    ├── test_audio_format_converter.cpp      # proto<->native 转换往返测试
    ├── test_ringbuffer.cpp
    ├── test_packet.cpp
    ├── test_udp_transport.cpp
    ├── test_nat_flow.cpp                    # NAT 握手 / 保活 / 路由 / 过期集成测试
    ├── test_data_flow.cpp                   # 端到端数据流（内存模拟 + 真实 UDP loopback）
    ├── test_session_lifecycle.cpp           # Session 严格生命周期 + 并发 + 边界
    └── test_jitter_buffer.cpp               # JitterBuffer 异常注入测试（乱序/丢包/重复/late/wrap）
    └── test_diagnostics.cpp                 # 诊断管理器测试（RTT/jitter/occupancy/underrun/loss）
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

当前正式控制协议（保活由 UDP HELLO 承担，gRPC 不含 KeepAlive RPC）：

```proto
syntax = "proto3";

package aqua.pb;

service AudioService {
  // 创建 session，返回 session_id + UDP endpoint + server AudioFormat
  rpc Connect(ConnectRequest) returns(ConnectResponse);
  // 删除 session
  rpc Disconnect(DisconnectRequest) returns(Empty);
}

message Empty {}

message ConnectRequest {
  // 可选，仅用于日志
  string client_name = 1;
}

message ConnectResponse {
  // SessionManager 生成的 ID
  uint32 session_id = 1;
  // UDP Server endpoint
  UdpEndpoint udp = 2;
  // Server 固定 AudioFormat
  AudioFormat audio_format = 3;
}

message UdpEndpoint {
  string address = 1;
  uint32 port = 2;
}

message DisconnectRequest {
  uint32 session_id = 1;
}

message AudioFormat {
  enum Encoding {
    ENCODING_INVALID  = 0;
    ENCODING_PCM_S16LE = 1;
    ENCODING_PCM_S32LE = 2;
    ENCODING_PCM_F32LE = 3;
    ENCODING_PCM_S24LE = 4;
    ENCODING_PCM_U8    = 5;
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
- C++23
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
- Disconnect
- 固定 UDP media port
- NAT endpoint 自动发现
- UDP HELLO 保活（刷新 NAT 映射 + server session last_seen）
- SessionManager 与 UDP Gateway 集成

目标：

**Client 位于一层 NAT 后仍然可以连接公网 Server。**

---

## Milestone 4：Stable PCM Playout

目标：

> **在固定播放延迟下，正确处理 UDP 的乱序、重复、有限丢包和 late packet，并持续向音频后端提供连续 PCM。**

实现：

- JitterBuffer
- 固定 target latency：30ms
- sequence reorder
- duplicate detection
- late packet detection
- playout deadline（到 deadline 才判定 lost，不因 sequence gap 立即判丢）
- packet loss detection
- silence fill（zero-fill）
- JitterBuffer → SPSC RingBuffer → WASAPI
- underrun / overrun statistics
- 网络异常注入测试

JitterBuffer 核心设计：

- push 时不立即判定 gap 为丢包
- 只有超过 packet playout deadline 才判定 lost
- late packet 超过 deadline 后丢弃
- 使用预分配连续 PCM storage（不使用每 Slot 一个 vector）
- 不在 JitterBuffer 内部依赖 timer（timer 是外部调度器）
- slot 空闲标记用 `bool valid`，不用 `sequence == 0`

不实现：

- 动态 target latency
- clock correction
- resampler
- frame slip
- Server diagnostic telemetry

---

## Milestone 5：Diagnostics & Buffer Policy

目标：

> **建立完整的网络、JitterBuffer、RingBuffer 和 Audio Backend 可观测性，为后续 clock drift / buffer correction 提供实测依据。**
>
> M5 的价值是把系统从"能播放"推进到"能解释为什么会这样播放"。

### 指标分层定义

指标按来源分层，**不得互相混淆**：

```text
Network layer
 ├─ RTT
 ├─ interarrival jitter
 ├─ packet loss（sequence gap，到 deadline 仍未到达）
 ├─ duplicate
 └─ late packet（超过 deadline 后到达）

JitterBuffer layer
 ├─ target latency
 ├─ current occupancy
 ├─ average / min / max occupancy
 ├─ startup latency（T0 首个 AudioPacket 到达 → T1 首个 PCM 提交 WASAPI）
 └─ playout deadline miss（pop 时包不存在，含 loss + overdue）

Audio pipeline layer
 ├─ RingBuffer occupancy（current / average / min / max）
 ├─ underrun（WASAPI read 返回不足）
 └─ overrun（JitterBuffer write 时 RingBuffer 空间不足）

Long-term behavior
 ├─ RingBuffer short-term occupancy slope（~5s 窗口）
 ├─ RingBuffer long-term occupancy slope（~60s 窗口）
 └─ estimated playback-rate drift（ppm，由 occupancy slope 推导）
```

**JitterBuffer occupancy ≠ RingBuffer occupancy**：前者描述"网络包到了多少"，后者描述"距离音频设备实际消耗还有多少 PCM"。

### packet loss 拆分

不只有一个 `packets_lost`，至少区分：

- `lost_packets`：到 deadline 仍未到达
- `late_packets`：超过 deadline 后到达
- `duplicate_packets`：重复包

这三者对分析网络问题至关重要。`100,102,101` 不是 loss（乱序），`100,103` 是 loss，`100,101,101` 是 duplicate。

### interarrival jitter

使用 RFC 3550 风格 EWMA：

```text
D(i,j) = (arrival_j - arrival_i) - (sample_position_j - sample_position_i) / sample_rate
J = J + (|D| - J) / 16
```

`sample_position` 承担 RTP timestamp 的角色。该指标是"端到端 packet arrival jitter"，包含网络抖动和 sender 调度抖动，不是纯网络链路 jitter。

### RingBuffer occupancy slope

M5 最重要的诊断数据不是 RTT，而是 **RingBuffer occupancy 的长期趋势**：

```text
d(buffer_occupancy) / dt → ppm
```

- `slope ≈ 0`：producer / consumer 频率一致
- `slope > 0`：producer > consumer（server 快于 client 播放）
- `slope < 0`：consumer > producer

短期波动（5s 窗口）反映网络 jitter；长期趋势（60s 窗口）反映真实 clock drift。

### clock drift 命名

使用 **estimated playback-rate drift**（而非 "clock drift"），因为当前测量的是 RingBuffer occupancy 变化趋势推断的 producer/consumer 速率差，不是直接测量两个物理晶振。

### RTT 不用于 clock drift

RTT 是 network/control-path diagnostic，**不是** clock synchronization。不设计 client_ts / server_ts 交叉比较。

### Server 不保存诊断数据

M5 诊断全部在 Client 本地完成。Server 只需 session state / last_seen / endpoint。真正的关键指标（JitterBuffer / RingBuffer / WASAPI）全部发生在 Client。

### target latency

- runtime configurable（`--jitter-latency <ms>` CLI 参数）
- 初期只支持手动调整
- 默认 30ms，可选 20/30/50/80ms
- 不自动调整

### Windows 可选：IAudioClock

Windows 平台可额外利用 `IAudioClock::GetPosition()` + `GetFrequency()` 测量 WASAPI device clock rate，与 server sample production rate 对比。比 RingBuffer slope 更直接。**仅诊断，不用于 correction。**

### 实验配置

M5 需建立可重复实验环境，至少支持：

- 固定 target latency：20 / 30 / 50 / 80ms
- 固定实验时长：5min / 30min / 2h / overnight
- 网络异常注入（loss / jitter / reorder / delay）

### 协议

- HELLO 保持简单（type + session_id）
- HELLO 不加入诊断 telemetry
- 后续确有 Server telemetry 需求时，再增加 REPORT packet

### clock correction

- 只检测，不自动修正
- 不使用 Resampler
- 不使用 frame slip
- correction strategy 根据 M5 实测数据决定

---

## Milestone 5+：Clock Correction Research

根据 M5 实测数据决定 correction strategy：

- Option A：小粒度 sample slip
- Option B：极低比例 time-scale modification
- Option C：平台特定时钟策略（WASAPI / PipeWire / AAudio）
- Option D：实测发现无需主动 correction

**不预设答案，用数据驱动决策。**

M5 完成后应能回答以下问题：

- 30ms buffer 到底够不够？
- 异常是网络 jitter、JitterBuffer 太小、RingBuffer 太小、WASAPI 调度、还是真实 sampling-rate offset？
- 是否需要主动 correction，还是靠 buffer 吸收即可？

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
  +-- Disconnect

UDP
  |
  +-- HELLO        (首次握手 + 周期保活)
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
| `for_each_connected(callback)`      | 共享锁   | 回调返回 `false` 停止遍历；禁止回调 SessionManager     |
| `clear()`                           | 排他锁   | 清空所有 session，返回被清理数量                       |

`collect_expired_sessions` **只读不删**，调用方拿到列表后自行 `remove_session`， 避免在持有共享锁时升级为排他锁。

## 22.3 audio_format

`src/core/public/audio_format.h`（见 §3.4）

POD 类型，无依赖，全 `noexcept` 接口。

## 22.4 net/transport

UDP socket 封装，基于 `asio::io_context`。

```cpp
namespace aqua::net {

class UdpTransport {
public:
    using ReceiveHandler = std::function<void(
        const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data)>;

    explicit UdpTransport(asio::io_context& ioc);
    ~UdpTransport();

    // 绑定本地端口。bind_ip "0.0.0.0" 监听所有接口。返回 false 表示绑定失败。
    bool bind(const std::string& bind_ip, std::uint16_t port);

    // 启动异步接收循环。handler 在 io_context 线程触发，禁止阻塞。
    void start_receive(ReceiveHandler handler);

    // 异步发送。通过 asio::post 调度到 io_context 线程，保证 socket 不被并发访问。
    // send 失败（如 ICMP port unreachable）降为 debug 日志，不回调调用方。
    void send(const asio::ip::udp::endpoint& target,
              std::span<const std::byte> data);

    void stop();
    bool is_open() const noexcept;
    asio::ip::udp::endpoint socket_local_endpoint() const;  // bind port=0 后查真实端口
};
}
```

约束：

- 不持有 SessionManager 引用；收到包后通过回调上交，由上层做路由。
- 接收缓冲预分配固定大小（65536 字节，覆盖最大 UDP datagram）， **不在回调中分配堆内存**。
- 回调在 io_context 线程执行，禁止阻塞；重活投递到其他线程。
- `send` 内部用 `asio::post` 调度到 io_context 线程，避免跨线程访问 socket。
- 接收循环遇到非 `operation_aborted` 错误（如 ICMP port unreachable / connection_refused） 不终止，继续投递
  `async_receive_from`，避免一个 client 关闭后影响其他 client。

## 22.5 net/packet

无状态编解码，纯函数式：

```cpp
namespace aqua::net {

enum class PacketType : std::uint8_t {
    Hello    = 1,
    HelloAck = 2,
    Audio    = 3,
};

#pragma pack(push, 1)
struct HelloPacket {
    PacketType type;          // 1 byte
    std::uint32_t session_id; // 4 bytes LE
};
static_assert(sizeof(HelloPacket) == 5);

struct AudioPacketHeader {
    PacketType type;               // 1 byte
    std::uint32_t session_id;      // 4 bytes LE
    std::uint32_t sequence;        // 4 bytes LE
    std::uint32_t sample_position; // 4 bytes LE
    std::uint16_t payload_size;    // 2 bytes LE
};
static_assert(sizeof(AudioPacketHeader) == 15);
#pragma pack(pop)

// 编码：返回写入字节数，out 空间不足返回 0
std::size_t encode_hello(std::uint32_t session_id, std::span<std::byte> out) noexcept;
std::size_t encode_hello_ack(std::uint32_t session_id, std::span<std::byte> out) noexcept;
std::size_t encode_audio(std::uint32_t session_id, std::uint32_t sequence,
                         std::uint32_t sample_position,
                         std::span<const std::byte> payload,
                         std::span<std::byte> out) noexcept;

// 解码：失败返回 std::nullopt
std::optional<PacketType>   peek_type(std::span<const std::byte> in) noexcept;
std::optional<HelloPacket>  decode_hello(std::span<const std::byte> in) noexcept;

struct DecodedAudio {
    AudioPacketHeader header;
    std::span<const std::byte> payload;  // 零拷贝，指向 in 内部
};
std::optional<DecodedAudio> decode_audio(std::span<const std::byte> in) noexcept;
}
```

约束：

- 所有整数按 **小端序** 读写（与 PCM 编码一致）。
- `#pragma pack(push, 1)` 保证结构体紧凑，无填充字节。
- 解码失败返回 `std::nullopt`，由调用方丢弃包。
- `decode_audio` 的 payload span 指向输入缓冲内部， **零拷贝**；调用方需在使用期间保持输入缓冲有效。
- 不做长度校验以外的语义校验（session_id 是否存在由上层判断）。
- `peek_type` 仅读首字节，用于快速分流，不校验长度。

## 22.6 grpc

`src/core/grpc/grpc_server.h` / `grpc_client.h`

```cpp
namespace aqua::grpc {

class AudioServiceImpl final : public pb::AudioService::Service {
public:
    AudioServiceImpl(SessionManager& sessions, AudioFormat server_format,
                     std::string udp_address, std::uint16_t udp_port);
    grpc::Status Connect(...) override;     // create_session + 返回 session_id/udp/format
    grpc::Status Disconnect(...) override;  // remove_session
};

class GrpcServer {
public:
    GrpcServer(SessionManager& sessions, AudioFormat server_format,
               std::string bind_ip, std::uint16_t rpc_port,
               std::string udp_address, std::uint16_t udp_port);
    void run();      // 阻塞，在单独线程中调用
    void shutdown(); // 非阻塞
    // 是否成功启动并仍在运行（server_->Wait() 尚未返回）。
    // 构造失败或 run() 已返回时返回 false。上层应在启动后检查此标志。
    bool is_running() const noexcept;
};

// Connect 返回结果
struct ConnectResult {
    std::uint32_t session_id;
    std::string udp_address;
    std::uint16_t udp_port;
    AudioFormat audio_format;
};

class GrpcClient {
public:
    bool connect_to_server(const std::string& server_ip, std::uint16_t rpc_port);
    bool connect(const std::string& client_name, ConnectResult& out);
    bool disconnect(std::uint32_t session_id);
};
}
```

约束：

- gRPC 服务持有 `SessionManager` 引用， **不拥有**它（生命周期由 main 管理）。
- `Connect` 内部调用 `sessions.create_session()`，把返回的 ID 与 UDP endpoint、AudioFormat 写入响应。
- `GrpcServer` 构造函数内同步调用 `BuildAndStart()`，失败时 `server_` 为空、`is_running()` 返回 false。 上层应在启动后轮询
  `is_running()`，失败则清理下层资源并退出。
- `GrpcClient::connect_to_server` 等待 channel 就绪最多 5 秒。
- 不在 gRPC 线程做网络 I/O 之外的工作。

## 22.7 audio/backend

抽象接口在 `src/core/audio/backend/audio_backend.h`，平台实现在子目录：

```cpp
namespace aqua::audio {

class CaptureBackend {
public:
    using CaptureCallback = std::function<void(std::span<const std::byte> pcm)>;
    virtual ~CaptureBackend() = default;

    // 启动采集。成功返回 true，并输出实际使用的 AudioFormat（WASAPI 使用设备 mix format）。
    // 阻塞直至初始化完成（成功或失败），便于调用方同步感知初始化错误。
    virtual bool start(CaptureCallback cb, AudioFormat& out_format) = 0;
    virtual void stop() = 0;
    // 采集线程是否仍在运行。初始化失败或运行时错误后返回 false。
    // 调用方应在主循环中轮询以感知运行时错误（如设备被移除）。
    virtual bool is_running() const = 0;
};

class PlaybackBackend {
public:
    using FillCallback = std::function<std::size_t(std::span<std::byte> out)>;
    virtual ~PlaybackBackend() = default;

    // 启动播放。成功返回 true。阻塞直至初始化完成（成功或失败）。
    // FillCallback 由播放线程调用，填充 out 缓冲，返回实际填充字节数；不足部分播放静音。
    virtual bool start(AudioFormat format, FillCallback cb) = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

// 工厂：平台相关，根据编译期宏选择 wasapi/pipewire/aaudio
std::unique_ptr<CaptureBackend> create_capture_backend();
std::unique_ptr<PlaybackBackend> create_playback_backend();
}
```

约束：

- 平台实现不得泄漏到接口（头文件不 include `<windows.h>` / `<mmdeviceapi.h>` 等）。
- `start()` 阻塞等待初始化结果（通过内部 `started_` 原子标志同步），便于调用方同步感知错误。
- 回调在音频实时线程触发，遵守 §10 / §15.2 约束（无锁、无分配、无阻塞）。
- 回调内只做 RingBuffer 写入， **不直接调用 UDP / SessionManager**。
- WASAPI 实现中 `ComPtr` 与 WAVEFORMATEX 互转函数提取到 `wasapi_common.h`，capture/playback 共用。
- `is_running()` 基于内部 `running_` 原子标志，线程因任何原因退出（含运行时错误）后返回 false。

## 22.8 ringbuffer

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
    std::size_t capacity() const noexcept;  // 总容量（已取整为 2 的幂）
    void clear() noexcept;                  // 仅在两端都停止时调用
};
}
```

约束：

- 容量向上取整为 2 的幂（最小 64 字节），使用 `std::bit_ceil`，便于掩码取模。
- 只允许 1 写 1 读；多生产者/消费者场景需外层串行化。
- 写满返回实际写入量（不阻塞、不覆盖未读数据），调用方负责丢弃或统计。
- `clear()` 非线程安全，仅在停止读写后调用（如 session 重连：stop audio → stop network → clear → restart）。
- `available_read()` / `available_write()` 是调度参考值，非强一致性快照，适合 `if (available >= ...)` 判断，不适合 `assert(...)`。
- `write_pos_` / `read_pos_` 使用 `alignas(64)` cache line 对齐，避免 SPSC 场景下的 false sharing。

## 22.9 jitter_buffer

`src/core/jitter_buffer/jitter_buffer.h`

```cpp
namespace aqua::jitter {

// Threading contract:
//   push() 和 pop_next() 必须在同一个 executor / 线程中调用。
//   当前设计为 io_context 单线程，push 来自 UDP 回调，pop_next 来自 steady_timer 回调。
//   内部不加锁。
class JitterBuffer {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // format:              音频格式（决定 frame_bytes）
    // frames_per_packet:   每包帧数（决定 payload_size 和 packet_duration）
    // target_latency_packets: 初始缓冲包数（如 3 包 = 30ms @ 10ms/包）
    // capacity_packets:    ring 容量，必须为 2 的幂，>= target_latency_packets * 2
    JitterBuffer(const AudioFormat& format,
                 std::uint32_t frames_per_packet,
                 std::size_t target_latency_packets,
                 std::size_t capacity_packets);

    // UDP I/O 线程调用：推入收到的音频包。
    // 自动归类：expected / future / duplicate / late。
    // push 时不判定丢包。
    void push(std::uint32_t sequence,
              std::uint32_t sample_position,
              std::span<const std::byte> payload);

    // 外部调度器查询下一次播放 deadline。
    // 返回 nullopt 表示尚未收到第一个包。
    [[nodiscard]] std::optional<time_point> next_playout_deadline() const noexcept;

    // deadline 到达后调用。输出 payload_size 字节：真实 PCM 或静音填充。
    // 返回 true 表示输出了真实 PCM，false 表示输出了静音（丢包）。
    [[nodiscard]] bool pop_next(std::span<std::byte> output);

    // 重置播放状态（slot + timeline）。不清除统计计数器。
    void reset();

    // ---- Diagnostics ----

    [[nodiscard]] std::uint64_t packets_received() const noexcept;
    [[nodiscard]] std::uint64_t packets_lost() const noexcept;
    [[nodiscard]] std::uint64_t duplicates() const noexcept;
    [[nodiscard]] std::uint64_t late_packets() const noexcept;
    [[nodiscard]] std::size_t   buffer_fill_packets() const noexcept;
    [[nodiscard]] std::uint32_t next_sequence() const noexcept;
};

}
```

约束：

- 内部按 `sequence` 排序，使用 playout deadline 判定丢包（不因 gap 立即判丢）。
- 预分配连续 PCM storage（`slots_` metadata + `storage_` PCM 分区），热路径零分配。
- slot 空闲标记用 `bool valid`，不用 `sequence == 0`。
- capacity 为 2 的幂，`>= target_latency_packets * 2`，构造函数验证否则抛异常。
- `packet_duration_` 由 `frames_per_packet` 和 `sample_rate` 推导，不硬编码。
- 播放时间线基于 `first_packet_time_ + target_latency * packet_duration_`，不使用计数式启动。
- `reset()` 只清除播放状态（slot + timeline），不清除 `storage_` 数据和统计计数器。
- `push` 和 `pop_next` 在同一线程（io_context）调用，内部无需锁。
- `target_latency` 由配置决定，不属于 AudioFormat。
- 不做重传（UDP 语义）。
- 不依赖 timer（timer 是外部调度器）。
- 调度器必须在 HELLO_ACK 后立即启动，不等待 WASAPI 初始化（见 §12.4）。

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

### Server 线程

| 线程            | 所属     | 职责                                 | 阻塞约束        |
|-----------------|----------|--------------------------------------|-----------------|
| Main            | main     | CLI 解析、构造对象、启动、主循环监控 | 可阻塞          |
| gRPC Server     | grpc     | `server_->Wait()` 阻塞，处理同步 RPC | 可阻塞          |
| UDP I/O         | asio     | `io_context.run()`，收发 UDP         | 不可阻塞        |
| Audio Capture   | 平台音频 | WASAPI loopback 采集回调             | 实时约束（§10） |
| Session Cleanup | session  | 周期扫描过期 session 并 remove       | 可阻塞          |
| Packetizer      | net      | 从 RingBuffer 读取 PCM → 编码 → 广播 | 可阻塞          |

### Client 线程

| 线程            | 所属     | 职责                                  | 阻塞约束        |
|-----------------|----------|---------------------------------------|-----------------|
| Main            | main     | CLI 解析、构造对象、主循环监控        | 可阻塞          |
| UDP I/O         | asio     | `io_context.run()`，收发 UDP + JitterBuffer push/pop + steady_timer 调度 | 不可阻塞 |
| Audio Playback  | 平台音频 | WASAPI 共享模式渲染回调               | 实时约束（§10） |
| HELLO Keepalive | net      | 每 1s 重发 HELLO 刷新 NAT + last_seen | 可阻塞          |

## 24.2 io_context 策略

- UDP 数据面使用 **单个** `asio::io_context`，可由 1~N 线程 `run()`。
- 第一阶段建议单线程 `run()`，避免包乱序与锁竞争。
- io_context 不与 gRPC 共享线程池。

## 24.3 跨线程通信路径

```text
Audio Capture Thread ──SPSC RingBuffer──> UDP I/O Thread ──UDP──> 远端
远端 ──UDP──> UDP I/O Thread(JitterBuffer.push) → (steady_timer) → JitterBuffer.pop_next ──SPSC RingBuffer──> Audio Playback Thread
gRPC Thread ──SessionManager(shared_mutex)──> UDP I/O Thread (查 endpoint)
```

JitterBuffer 的 `push` 和 `pop_next` 都在同一个 io_context 线程执行，无需锁。
跨线程边界仅在 JitterBuffer → RingBuffer（SPSC）和 RingBuffer → WASAPI（SPSC）。

允许的跨线程共享：

- `SessionManager`（通过 `shared_mutex`）
- `SpscRingBuffer`（无锁，单写单读）
- `std::atomic` 标志位（如 `running_`）

禁止的跨线程共享：

- 直接共享 `asio::ip::udp::socket`（必须经 `io_context.post()` 调度）
- 在音频回调线程访问 SessionManager（锁会阻塞实时线程）
- 在 UDP 回调中直接调用阻塞 gRPC

## 24.4 退出与关闭顺序

### Server 关闭顺序

```text
1. main 收到 SIGINT 或检测到 capture/grpc 异常，置 g_running = false
2. capture->stop()              // 停止采集线程
3. grpc_server.shutdown()       // 通知 gRPC 停止
4. transport.stop()             // 关闭 UDP socket
5. ioc.stop()                   // 停止 io_context
6. join: grpc_thread / ioc_thread / cleanup_thread / sender_thread
7. sessions.clear()             // 清理残留 session
8. 析构对象（逆序）
```

### Client 关闭顺序

```text
1. main 收到 SIGINT 或检测到 playback 异常 / 数据接收超时，置 g_running = false
2. playback->stop()             // 停止播放线程
3. transport.stop()             // 关闭 UDP socket
4. ioc.stop()                   // 停止 io_context
5. join: ioc_thread / hello_keepalive_thread
6. grpc_client.disconnect()     // 优雅断开 gRPC session
7. 析构对象（逆序）
```

### 主循环健康监控

Server/Client 主循环每 100ms 轮询以下健康标志，任一异常即触发优雅退出：

- `capture->is_running()` / `playback->is_running()`：音频后端线程存活
- `grpc_server.is_running()`（仅 server）：gRPC 服务存活
- Client 数据接收超时：超过 `CLIENT_AUDIO_TIMEOUT`（5s）未收到 Audio 包 → server 已断开

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

- **Server 侧 session 超时**：超过 `UDP_SESSION_TIMEOUT`（5s）未收 HELLO → server 清理线程 `remove_session`。
- **Client 侧数据接收超时**：超过 `CLIENT_AUDIO_TIMEOUT`（5s）未收到 Audio 包 → 认为 server 已断开，优雅退出。
- **Client 播放后端错误**：检测到 `playback->is_running() == false`（如 WASAPI 设备被占用/移除）→ 优雅退出。
- **Server 采集/gRPC 错误**：检测到 `capture->is_running() == false` 或 `grpc_server.is_running() == false` → 优雅退出。
- 退出前 client 尝试 `grpc_client.disconnect()`（server 已关则失败，仅记 warn）。
- 重连后重新 `Connect`，获取新 session_id，重新 UDP 握手。
- 指数退避重连（后续实现，当前阶段未做）。

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

UDP 端口不由 CLI 指定，由 gRPC Connect 响应返回（见 §6.3）。

## 26.3 Server AudioFormat 配置

Server 启动时由 WASAPI loopback 设备 mix format 决定（通常 `PcmF32LE / 48000 / 2ch` 或
`PcmS16LE / 48000 / 2ch`，因设备而异）。运行期不可变。后续可加 `--audio-format` CLI 参数覆盖。

## 26.4 超时参数

| 参数                     | 默认   | 说明                                                       |
|--------------------------|--------|------------------------------------------------------------|
| UDP_SESSION_TIMEOUT      | 5 s    | `collect_expired_sessions` 阈值（仅 HELLO 刷新 last_seen） |
| KEEPALIVE_INTERVAL       | 1 s    | Client 重发 HELLO 频率（须 < timeout/2，5s/1s = 5 次机会） |
| EXPIRED_CLEANUP_INTERVAL | 2 s    | Server 扫描周期                                            |
| HELLO_RETRY_INTERVAL     | 800 ms | Client 握手阶段 HELLO 重试间隔                             |
| CLIENT_AUDIO_TIMEOUT     | 5 s    | Client 无 Audio 包超时，认为 server 已断开                 |

这些常量集中在 `src/core/public/config.h`，不散落各处。

## 26.5 不引入配置文件

第一阶段所有配置走 CLI + 编译期常量。 **不引入** YAML / JSON / TOML 配置文件，避免增加解析依赖与路径搜索复杂度。

---

# 27. 日志规范

## 27.1 级别使用

| 级别  | 使用场景                                             |
|-------|------------------------------------------------------|
| Trace | UDP 逐包来源/字节数（默认关闭）                      |
| Debug | 状态机迁移、endpoint 变更、HELLO keepalive、周期统计 |
| Info  | 服务启动 / 停止、新 session 建立、WASAPI 启动/停止   |
| Warn  | 丢包、解码失败、端口重试、Disconnect 找不到 session、JitterBuffer sequence jump reset |
| Error | gRPC 失败、bind 失败、设备打开失败、后端异常退出     |

### 周期性统计日志（Debug 级别）

为避免日志刷屏，高频路径采用 5 秒周期统计输出：

- WASAPI capture：包数 / 字节数 / 速率（packets/s, KB/s）
- WASAPI playback：回调数 / 填充字节数 / 静音字节数 / 填充率
- Server packetizer：发送包数 / 字节数 / 速率 / 活跃 session 数
- Client 接收：音频包数 / 字节数 / HELLO_ACK 数 / 速率
- JitterBuffer：sequence jump reset（Warn，仅异常时触发，非常规路径）

### 不记录日志的高频路径

- `SessionManager::for_each_connected()`：被 packetizer 每秒调用约 100 次，内部不记日志
- `SessionManager::establish_udp()`：HELLO keepalive 路径，由上层 UDP 回调记录
- UDP send 失败：降为 debug，避免 ICMP 错误刷屏
- `JitterBuffer::push()` / `pop_next()`：热路径，每秒调用约 100 次，内部不记日志（仅在 sequence jump reset 时记 Warn）

## 27.2 必含字段

每条日志应能定位：

- `session_id`（若有，以 `0x{:08X}` 格式）
- `endpoint`（若有，`IP:port` 格式）
- `sequence`（音频包相关）

格式示例：

```text
[info] Session 0x8DC0FA26 UDP established: 127.0.0.1:59745
[debug] Session 0x8DC0FA26 HELLO keepalive from 127.0.0.1:59745
[debug] WASAPI capture stats: 500 packets, 1920000 bytes in 5.00s (100.0 packets/s, 374.9 KB/s)
```

## 27.3 默认级别

- Debug preset（`AQUA_DEBUG=ON`）：`Debug`（编译期由 `default_log_level()` 返回，main 启动时设置）
- Release preset：`Info`
- 调试时通过 CLI（后续加 `--log-level`）或环境变量切换。
- 日志格式：`[YYYY-MM-DD HH:MM:SS.mmm] [level] message`

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

| 模块                   | 测试文件                        | 测试重点                                             |
|------------------------|---------------------------------|------------------------------------------------------|
| logger                 | test_log.cpp                    | 级别切换、格式化不抛异常                             |
| session_manager        | test_session_manager.cpp        | 创建/删除/握手/超时/计数/状态迁移/并发               |
| session_lifecycle      | test_session_lifecycle.cpp      | 严格状态转换/幂等/边界/并发压力/析构安全             |
| cli_parser             | test_cli_parser_*.cpp           | 默认值、自定义、help/version、非法端口、位置参数拒绝 |
| audio_format           | test_audio_format.cpp           | bytes_per_sample / frame_bytes 各编码                |
| audio_format_converter | test_audio_format_converter.cpp | proto<->native 往返、所有编码、极值、多次稳定        |
| ringbuffer             | test_ringbuffer.cpp             | 读写指针、写满不覆盖、空读、wraparound、并发         |
| net/packet             | test_packet.cpp                 | 编解码对称性、截断包、字节级 wire format             |
| net/transport          | test_udp_transport.cpp          | bind/close、loopback 收发、ICMP 恢复                 |
| nat_flow               | test_nat_flow.cpp               | NAT 握手/保活/广播/重映射/丢包/超时                  |
| data_flow              | test_data_flow.cpp              | 端到端数据流（内存+UDP）、背压、大 payload           |
| jitter_buffer          | test_jitter_buffer.cpp          | 正常/乱序/丢包/连续丢包/重复/late-on-time/late-missed-deadline/sequence wrap/huge jump/startup |
| diagnostics            | test_diagnostics.cpp            | RTT/interarrival jitter/RB occupancy/underrun/loss+late snapshot |

当前共 **160 个测试**，全部通过。

## 29.2 测试约束

- 测试不得依赖音频硬件（WASAPI 不在测试范围）。
- 涉及 asio endpoint 的测试用 `127.0.0.1` + 随机高端口（`bind("127.0.0.1", 0)`）。
- 超时测试用真实 `sleep_for`，时长保持 < 3s，避免拖慢 CI。
- 测试文件命名 `test_<module>.cpp`，镜像 `src/` 布局。
- 真实 UDP 测试允许丢包，断言用 `EXPECT_GE` 而非精确匹配。

## 29.3 集成测试

已实现的端到端测试（`test_data_flow.cpp`）：

- **内存模拟**：capture callback → ringbuffer → packetize → broadcast → client decode → ringbuffer → playback，字节级校验
- **真实 UDP loopback**：两个 UdpTransport 通过 127.0.0.1 收发，验证 HELLO 握手 + ACK + Audio 传输 + 大 payload
- **完整握手 + 广播流程**：SessionManager + UdpTransport 串联，模拟 client connect → hello → ack → server broadcast

不做跨进程测试，避免 CI 复杂度。

---

# 30. 实现状态

> 本节追踪当前实现进度，与 §18 Milestone 对应。每次合入需更新。

## 30.1 已完成

### Milestone 0：工程基础

- ✅ **CMake + vcpkg manifest + presets**（win/linux/macos）
- ✅ **C++23** 标准强制
- ✅ **logger**：spdlog 薄封装，5 级日志 + 格式化接口 + `default_log_level()`（AQUA_DEBUG 宏控制）
- ✅ **session_manager**：create / remove / get / establish_udp / touch / is_connected / collect_expired / count /
  for_each_connected / clear
- ✅ **SessionState 状态机**：Created / Connected（Connecting/Expired/Closed 保留未用）
- ✅ **audio_format**：原生 `AudioFormat` + `AudioEncoding`，与 proto 同步
- ✅ **audio_format_converter**：`pb::AudioFormat <-> aqua::AudioFormat` 双向转换 + 往返测试
- ✅ **proto**：`AudioService`（Connect / Disconnect）+ `AudioFormat` + `UdpEndpoint`（保活由 UDP HELLO 承担，无 KeepAlive
  RPC）
- ✅ **CLI**：`cli_parser_server` / `cli_parser_client` + `cli_parser_common.h`（parse_port 共用）
- ✅ **UDP Transport**：Asio 封装，异步收发，预分配接收缓冲，ICMP 错误恢复，回环测试通过
- ✅ **SPSC RingBuffer**：无锁环形缓冲，容量取整 2 的幂，clear / capacity，并发读写测试通过

### Milestone 1：Windows PCM

- ✅ **Packet codec**：Hello / HelloAck / Audio 二进制编解码，小端序，`#pragma pack` 紧凑，零拷贝解码 payload
- ✅ **WASAPI Loopback 采集**：COM RAII（ComPtr 提取到 wasapi_common.h），自动 mix format 探测，polling 模式，`started_` 同步初始化
- ✅ **WASAPI 播放**：共享模式渲染，FillCallback 回调填充 + 静音填充，`started_` 同步初始化
- ✅ **Audio Backend 抽象**：`CaptureBackend` / `PlaybackBackend` 接口（含 `is_running()`）+ 工厂函数
- ✅ **Server 端到端**：WASAPI 采集 → RingBuffer → Packetizer（10ms/包）→ UDP 发送
- ✅ **Client 端到端**：HELLO → UDP 接收 → depacketize → RingBuffer → WASAPI 播放
- ✅ **单元测试**：logger / session_manager / cli_parser / audio_format / audio_format_converter / ringbuffer / packet /
  udp_transport

### Milestone 2：SessionManager 集成

- ✅ **SessionManager 接入 UDP 握手**：server 收到 HELLO 后调用 `establish_udp()` 记录 NAT 真实 endpoint，状态 Created →
  Connected
- ✅ **HELLO_ACK 响应**：`encode_hello_ack()` 编码，server 握手成功后立即回复
- ✅ **音频路由**：`SessionManager::for_each_connected()` 遍历 Connected 会话，packetizer 向所有已连接 client 广播音频包
- ✅ **Session 超时清理**：独立清理线程周期调用 `collect_expired_sessions()` + `remove_session()`
- ✅ **替换 M1 简化逻辑**：移除 "最后发 HELLO 的客户端 endpoint" hack，使用真实 session_id 路由

### Milestone 3：gRPC + NAT

- ✅ **config.h**：集中超时 / 保活常量（UDP_SESSION_TIMEOUT / KEEPALIVE_INTERVAL / EXPIRED_CLEANUP_INTERVAL /
  HELLO_RETRY_INTERVAL / CLIENT_AUDIO_TIMEOUT）
- ✅ **GrpcServer**：`AudioServiceImpl`（Connect / Disconnect）+ `GrpcServer` 生命周期包装 + `is_running()` 健康检测
- ✅ **GrpcClient**：connect_to_server / connect / disconnect
- ✅ **Connect 返回完整信息**：session_id + UDP endpoint + AudioFormat（ **音频格式经 gRPC 传输，UDP 包不含格式信息**）
- ✅ **NAT endpoint 自动发现**：server 以 HELLO 包 source endpoint 为准，不依赖 client 上报本地地址
- ✅ **固定 UDP media port**：单一 UDP 端口服务所有 session，按 session_id 路由
- ✅ **UDP HELLO 单路保活**：client 每 1s 重发 HELLO，server 收到后 establish_udp（幂等）+ touch_session + 回复 HELLO_ACK
- ✅ **Client 完整流程**：gRPC Connect → UDP HELLO → HELLO_ACK → WASAPI 播放 + HELLO 保活线程 → Disconnect
- ✅ **Server 完整流程**：gRPC + SessionManager + UDP 接收 + packetizer 广播 + 超时清理
- ✅ **端到端回环验证**：本机 server + client，gRPC 建连 + UDP 握手 + 音频回放正常
- ✅ **后端错误监控**：`is_running()` 暴露线程存活状态，主循环轮询以感知 WASAPI 初始化失败/运行时错误
- ✅ **Server 退出清理**：`sessions.clear()` 消除析构 warning

### 后续增强（M3 之后）

- ✅ **代码去重**：WASAPI ComPtr + format 转换提取到 `wasapi_common.h`，CLI `parse_port` 提取到 `cli_parser_common.h`
- ✅ **日志增强**：WASAPI/packetizer/client 周期性统计（5s），gRPC 入口日志，UDP trace 级别逐包日志
- ✅ **Client 数据接收超时**：`CLIENT_AUDIO_TIMEOUT`（5s）未收到 Audio 包 → server 已断开 → 优雅退出
- ✅ **gRPC 启动失败检测**：`GrpcServer::is_running()` 让 server_main 感知构造失败并清理
- ✅ **严格测试**：新增 test_audio_format_converter（11）/ test_data_flow（11）/ test_session_lifecycle（23）/ test_jitter_buffer（17）/ test_diagnostics（6），共 160 个测试
- ✅ **WasapiCapture 统一 started_ 模式**：与 WasapiPlayback 一致，通过 `started_` 原子标志同步初始化结果

### Milestone 4：Stable PCM Playout

- ✅ **JitterBuffer 核心实现**：playout deadline 丢包判定（不因 gap 立即判丢）、sequence reorder、duplicate detection、late packet detection、silence fill
- ✅ **预分配连续 storage**：slots metadata + storage PCM 分区，热路径零 heap allocation，slot 空闲用 `bool valid`
- ✅ **sequence 回绕处理**：int32_t 有符号差值比较
- ✅ **JitterBuffer → RingBuffer 串联**：steady_timer 外部调度器驱动 pop_next → ringbuffer.write，WASAPI callback 路径零改动
- ✅ **固定 target latency**：30ms（3 包 × 10ms），不自动调整
- ✅ **异常注入测试**：17 个测试覆盖正常/乱序/单丢包/连续丢包/重复/late-on-time/late-missed-deadline/sequence wrap/huge jump/startup/payload mismatch/continuous operation/buffer fill/reset+stats/next_sequence/output too small/capacity validation

### Milestone 5：Diagnostics & Buffer Policy

- ✅ **DiagnosticsManager**：Client 本地诊断数据采集，分层指标（Network / JitterBuffer / RingBuffer / Drift）
- ✅ **RTT 测量**：HELLO → HELLO_ACK 时间差，EWMA 平滑
- ✅ **interarrival jitter**：RFC 3550 风格 EWMA，sample_position 作为 timestamp
- ✅ **packet loss 拆分**：lost / late / duplicate 分别统计
- ✅ **JitterBuffer occupancy**：current / avg / min / max 水位
- ✅ **RingBuffer occupancy**：current / avg / min / max 水位
- ✅ **underrun 计数**：WASAPI read 不足时递增
- ✅ **playback-rate drift**：RingBuffer occupancy slope（short 5s + long 60s 窗口线性回归 → ppm）
- ✅ **`--jitter-latency <ms>` CLI 参数**：手动调整 target latency（默认 30，可选 20/30/50/80）
- ✅ **周期性诊断日志**：每 5s 输出完整诊断快照
- ✅ **测试**：6 个诊断测试覆盖 RTT / jitter / occupancy / underrun / loss+late

## 30.2 当前位置

**Milestone 0 + 1 + 2 + 3 + 4 已完成，Milestone 5 已完成核心诊断功能。**

M4 实现了 JitterBuffer（playout deadline、预分配连续 storage、push/pop 语义），
接入客户端数据流（JitterBuffer → RingBuffer → WASAPI），实机回环测试通过。

M5 实现了 DiagnosticsManager（RTT / interarrival jitter / loss / occupancy / drift slope），
`--jitter-latency` CLI 参数，周期性诊断日志输出。待实机长时间运行验证 drift 数据。

```text
Client --gRPC Connect----> Server  (返回 session_id + UDP endpoint + AudioFormat)
Client --UDP HELLO-------> Server  (记录 NAT endpoint, 状态 Created → Connected)
Client <--HELLO_ACK------ Server
Server --UDP AUDIO-------> Client (按 session 路由, 向所有 Connected 会话广播)
Client --UDP HELLO-------> Server  (每 1s 保活: 刷新 NAT 映射 + session last_seen)
Client --gRPC Disconnect-> Server
```

运行方式（使用 debug preset，日志默认 Debug 级别）：

```bash
# 构建
cmake --preset windows-x64-debug
cmake --build cmake_build/windows-x64-debug --config Debug

# Server
aqua_server --bind-ip 0.0.0.0 --rpc-port 50051 --udp-port 50000

# Client（UDP 端口由 gRPC Connect 返回，无需 CLI 指定）
aqua_client --server-ip <server_ip> --server-rpc-port 50051

# 测试
ctest --test-dir cmake_build/windows-x64-debug -C Debug --output-on-failure
```

## 30.3 下一步优先级

1. **M5+ Clock Correction Research**：根据 M5 实测 drift 数据决定 correction strategy
2. **M6 跨平台**：PipeWire / AAudio / Qt6 / Kotlin+JNI

## 30.4 已知偏差与遗留

- **M1 无 session 管理**（M2 已修复）：接入 SessionManager，移除 "最后发 HELLO 的客户端" hack，使用真实 session_id 路由。
- **M1 无格式协商**（M3 已修复）：通过 gRPC Connect 返回服务器 AudioFormat，client 使用该格式播放。 **音频格式信息经 gRPC
  传输，UDP 包不含格式信息**。
- **无 Jitter Buffer**（M4 已完成）：已接入 JitterBuffer，支持 playout deadline 丢包判定、乱序重排、去重、late packet 检测、静音填充。
- **无 client 端格式转换**：当前 client 直接用 server 返回的格式播放；若 client 设备不支持需后续实现转换（§14）。
- **无断连重连**：session 过期或数据接收超时后客户端退出，未实现自动指数退避重连（§25.3）。后续加入。
- **无 --log-level CLI**：当前通过 `AQUA_DEBUG` 编译期宏控制默认级别，后续加 CLI 参数。
- **线程模型未优化**：server 当前有 4 个线程（gRPC/UDP/cleanup/packetizer），用户提出可精简为 gRPC+UDP 两个线程（cleanup 可放入
  asio），暂未调整。
- **sample_position 截断**：`AudioPacketHeader.sample_position` 为 `uint32_t`，48kHz 下约 24.8 小时回绕。M4/M5 不动包头，
  后续协议版本改为 `uint64_t`。
- proto `AudioFormat.Encoding` 曾存在 `S32LE=3 / F32LE=2` 的值互换 bug，已修正。
- `include/aqua.h`（C API）尚未创建，待 M6 引入 UI 时再建。