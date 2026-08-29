# 源码—文档对照表

本页用于维护者从源码进入设计文档。原则是：**先看模块文档，再读对应 `.h`，最后用 `.cpp` 验证实现细节。**

| 源码 | 设计文档 | 主要责任 |
|---|---|---|
| `audio/audio_format.h` | `audio_model.md` / `devices_and_format.md` | PCM 格式、frame/byte 几何 |
| `audio/audio_frame.h` | `audio_model.md` | 固定帧视图与 well-formed |
| `audio/audio_block.h` | `audio_model.md` | capture callback 借用 block |
| `audio/buffer/jitter_buffer.*` | `jitter_buffer.md` / `buffer_design.md` | 唯一 Client playback buffer |
| `audio/packetizer/*` | `packetizer.md` | 可变 capture block → 固定 frame |
| `audio/queue/audio_frame_queue.h` | `audio_frame_queue.md` | capture RT → network worker |
| `audio/capture/*` | `capture.md` / `wasapi.md` | 平台采集 backend |
| `audio/playback/*` | `playback.md` / `wasapi.md` | 平台播放 backend |
| `audio/devices/*` | `devices.md` | 设备枚举/默认/解析 |
| `audio/audio_format_converter.*` | `audio_format_converter.md` | Proto ↔ Core 格式转换 |
| `diagnostics/*` | `observability.md` / `diagnostics.md` | 运行统计快照 |
| `logger/*` | `observability.md` | spdlog 与平台 sink |
| `net/udp/network_frame.*` | `protocol.md` / `udp.md` | Aqua UDP wire format |
| `net/udp/udp_transport.*` | `udp_transport.md` | strand、socket、send queue |
| `net/udp/udp_client.*` | `udp.md` | Client HELLO/ACK/audio |
| `net/udp/udp_server.*` | `udp.md` | Server session endpoint/broadcast |
| `net/session_manager.*` | `session.md` | session ID、endpoint、expiry |
| `net/grpc/*` | `grpc.md` | control plane |
| `net/proto/*` | `proto_boundary.md` | wire → domain conversion |
| `runtime/client_runtime.*` | `runtime.md` / `threading_and_lifecycle.md` | Client 生命周期与组装 |
| `runtime/server_runtime.*` | `runtime.md` / `threading_and_lifecycle.md` | Server 生命周期与组装 |
| `runtime/audio_network_dispatcher.*` | `server_audio_path.md` | queue → encoded datagram → broadcast |
| `audio/*/factory.*` | `factories.md` | 平台 backend 选择 |
| `audio/playback/wasapi_audio_playback.*` | `wasapi.md` | Windows playback |
| `audio/capture/wasapi_audio_capture.*` | `wasapi.md` | Windows capture |

## 阅读顺序

### 新维护者

`README → architecture → audio_model → jitter_buffer → protocol → runtime → threading_and_lifecycle → 对应模块`

### 调试网络播放

`runtime → grpc → session → udp_transport → udp → jitter_buffer → diagnostics`

### 调试 Server 音频

`wasapi(capture) → packetizer → audio_frame_queue → server_audio_path → udp`

### 新增 Android backend

`playback → factories → devices → android_roadmap → C API/JNI 边界`
