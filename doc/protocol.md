# 协议设计

## 1. Audio Format

### Server 固定 AudioFormat

Server 启动时确定一个 `AudioFormat`（如 PCM S16LE / 48000 Hz / 2ch），运行期间不变；Client 通过 `Connect` 获取。

Server 不负责：重采样、声道转换、Sample Format 转换、Codec 转换。Client 若设备格式不匹配，由 Client 自己转换。

### 原生 AudioFormat（C++ 侧）

`src/core/public/audio_format.h`：

```cpp
enum class AudioEncoding : std::uint8_t {
    Invalid = 0, PcmS16LE = 1, PcmS32LE = 2, PcmF32LE = 3, PcmS24LE = 4, PcmU8 = 5,
};

struct AudioFormat {
    AudioEncoding encoding = AudioEncoding::Invalid;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    bool valid() const noexcept;
    std::uint32_t bytes_per_sample() const noexcept;
    std::uint32_t frame_bytes() const noexcept; // bytes_per_sample * channels
};
```

- `AudioEncoding` 数值必须与 proto `AudioFormat.Encoding` 一一对应，修改任一端必须同步另一端。
- 原生结构只含 POD 字段，可自由拷贝，不持有资源。

`AudioFormat` 只描述 **音频数据本身**，不包含 frame_samples / packet_size / jitter_buffer_size / target_latency
等传输或播放策略字段。

## 2. gRPC Control Plane

### 职责

当前只提供 `Connect` / `Disconnect`， **不提供** GetAudioFormat / SetAudioFormat / KeepAlive / Codec 协商 /
RegisterMediaEndpoint。

- 不提供 KeepAlive：保活完全由 UDP HELLO 承担（同时刷新 NAT 映射 + server session last_seen），gRPC 保活会制造「session 活但
  NAT 映射死」的隐蔽状态。
- 不提供 GetAudioFormat：Server 格式固定，Connect 已返回 AudioFormat，UDP endpoint 也无需二次 RPC 注册。

### Connect / Disconnect

`Connect` 返回 `session_id + udp endpoint + audio_format`；`Disconnect` 删除 session。

### Session ID

`using session_id_t = std::uint32_t;`，只用于区分 session、UDP 路由、SessionManager 查表， **不作为**身份 / 设备 ID / 凭据 /
Token。

推荐结构：16 bit 随机 instance + 16 bit 自增 counter（`7A31-0001`），仅需在 Server 生命周期内尽量不冲突。

## 3. NAT Traversal

当前只实现 **Client 在一层 NAT 后，Server 有公网 UDP 地址**；不实现双方 NAT / 对称 NAT / STUN / TURN / ICE。

### Server 端口

- TCP / gRPC（如 50051）
- UDP / Media（如 50000，单一固定端口， **不为每个 session 分配端口**，按 session_id 路由）

### 建立流程

```text
Client --gRPC Connect--> Server  (返回 session_id + UDP endpoint + AudioFormat)
Client --UDP HELLO--> Server     (记录 NAT 后真实 source endpoint, Created -> Connected)
Client <--HELLO_ACK-- Server
Server --UDP AUDIO--> Client     (向所有 Connected session 广播)
Client --UDP HELLO--> Server     (每 1s 保活: 刷新 NAT 映射 + last_seen)
```

- Server 以 UDP 包的实际 source endpoint 为准（NAT 映射后的地址），不信任 client 上报的本地地址。
- Server 收到合法 HELLO 后立即回 HELLO_ACK，Client 收到 ACK 认为 UDP 通道建立。

### HELLO 单路保活

HELLO 兼任两种角色：首次握手（Created → Connected）+ 周期保活（每 1s）。Server 收到后 `establish_udp()`（幂等，更新 endpoint +
last_seen）+ 回 ACK。

参数关系：`SESSION_TIMEOUT = 5s`，`HELLO_KEEPALIVE_INTERVAL = 1s`（5 次保活机会，容忍连续 4 次丢包）。

**server 只在 HELLO 上 `touch_session`，不在 Audio 包上更新 last_seen**（否则恶意 client 持续发 Audio 包会让 session
永不过期）。

## 4. UDP Packet

### 基本原则

UDP 不用 protobuf，音频走自定义二进制 packet（减少包头 / CPU / 分配，便于预分配 buffer）。

### 包类型

```cpp
enum class PacketType : std::uint8_t {
    Hello    = 1,
    HelloAck = 2,
    Audio    = 3,
};
```

### Hello / HelloAck

```cpp
#pragma pack(push, 1)
struct HelloPacket {
    PacketType type;          // 1 byte
    std::uint32_t session_id; // 4 bytes LE
};
static_assert(sizeof(HelloPacket) == 5);
#pragma pack(pop)
```

### Audio

```cpp
#pragma pack(push, 1)
struct AudioPacketHeader {
    PacketType type;               // 1 byte
    std::uint32_t session_id;      // 4 bytes LE
    std::uint32_t sequence;        // 4 bytes LE
    std::uint32_t sample_position; // 4 bytes LE（~24.8h 回绕 @48kHz）
    std::uint16_t payload_size;    // 2 bytes LE
};
static_assert(sizeof(AudioPacketHeader) == 15);
#pragma pack(pop)
```

字段语义：

- `session_id`：Server 查表路由；`0` 为广播哨兵（`net::kBroadcastSessionId`，SessionManager 保证不生成 0）。
- `sequence`：判重 / 判序 / 判丢包 / JitterBuffer 排序。UDP 不重传。
- `sample_position`：音频时间轴（帧计数）。`uint32_t` 在 48kHz 下约 24.8h 回绕；接收方按模运算处理。
- `payload_size`：接收端按 `AudioFormat + payload_size` 计算实际 sample 数。

约束：

- 所有整数按 **小端序**读写（`packet.cpp` 有 `static_assert(std::endian::native == std::endian::little)` 防御）。
- 解码失败返回 `std::nullopt`，调用方丢包。
- `decode_audio` 的 payload span 指向输入缓冲内部（零拷贝），调用方须在使用期间保持输入有效。
- `peek_type` 只读首字节快速分流，不校验长度。

## 5. gRPC Proto

```proto
syntax = "proto3";
package aqua.pb;

service AudioService {
  rpc Connect(ConnectRequest) returns(ConnectResponse);
  rpc Disconnect(DisconnectRequest) returns(Empty);
}

message Empty {}

message ConnectRequest { string client_name = 1; }

message ConnectResponse {
  uint32 session_id = 1;
  UdpEndpoint udp = 2;
  AudioFormat audio_format = 3;
}

message UdpEndpoint { string address = 1; uint32 port = 2; }

message DisconnectRequest { uint32 session_id = 1; }

message AudioFormat {
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
