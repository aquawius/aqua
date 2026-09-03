# 源码—文档对照表

维护者从源码进入设计文档的索引。原则：**先看模块文档，再读对应 `.h`，最后用 `.cpp` 验证实现细节。**

| 源码                                        | 设计文档                                          | 主要责任                             |
|-----------------------------------------------|-----------------------------------------------------|--------------------------------------|
| `audio/audio_format.h`                        | `audio_model.md` / `devices_and_format.md`          | PCM 格式、frame/byte 几何            |
| `audio/audio_frame.h`                         | `audio_model.md`                                    | 固定帧视图与 well-formed 判定        |
| `audio/audio_block.h`                         | `audio_model.md`                                    | capture 回调借用的变长 block         |
| `audio/audio_error.h`                         | `audio_model.md`                                    | 音频错误分类与名称                   |
| `audio/audio_switch_result.h`                 | `playback_switching_design.md` §9                   | 两侧共享的切换结果词汇               |
| `audio/buffer/jitter_buffer.*`                | `jitter_buffer.md` / `buffer_design.md`             | Client 唯一的播放缓冲                |
| `audio/packetizer/*`                          | `packetizer.md`                                     | 变长 capture block → 定长 frame      |
| `audio/queue/audio_frame_queue.h`             | `audio_frame_queue.md`                              | capture RT → network worker（仅头文件）|
| `audio/capture/audio_capture.h`               | `capture.md`                                        | 采集后端抽象与回调契约               |
| `audio/capture/capture_manager.*`             | `capture_switching_design.md`                       | Server 采集端点切换事务              |
| `audio/capture/wasapi/*`                      | `capture.md` / `wasapi.md`                          | Windows 采集后端                     |
| `audio/playback/audio_playback.h`             | `playback.md`                                       | 回放后端抽象与回调契约               |
| `audio/playback/playback_manager.*`           | `playback_switching_design.md`                      | Client 回放流切换事务                |
| `audio/playback/wasapi/*`                     | `playback.md` / `wasapi.md`                         | Windows 回放后端                     |
| `audio/playback/aaudio/*`                     | `playback.md` / `aaudio_backend_design.md`          | Android 回放后端                     |
| `audio/devices/*`                             | `devices.md` / `devices_and_format.md`              | 设备枚举、默认设备、按 id 解析       |
| `audio/audio_format_converter.*`              | `audio_format_converter.md`                         | Proto ↔ Core 格式转换                |
| `diagnostics/*`                               | `observability.md` / `diagnostics.md`               | 运行统计快照与诊断输出               |
| `logger/*`                                    | `observability.md`                                  | spdlog 封装与平台 sink               |
| `net/udp/network_frame.*`                     | `protocol.md` / `udp.md`                            | Aqua UDP wire 编解码                 |
| `net/udp/udp_transport.*`                     | `udp_transport.md`                                  | strand、socket、发送队列             |
| `net/udp/udp_client.*`                        | `udp.md`                                            | Client HELLO / ACK / audio 接收      |
| `net/udp/udp_server.*`                        | `udp.md`                                            | Server session endpoint / broadcast  |
| `session/session_manager.*`                   | `session.md`                                        | session id、endpoint、过期回收       |
| `net/grpc/*`                                  | `grpc.md`                                           | 控制面                               |
| `proto/aqua_service.proto`                    | `proto_boundary.md`                                 | wire ↔ domain 转换                   |
| `runtime/client_runtime.*`                    | `runtime.md` / `threading_and_lifecycle.md`         | Client 生命周期与组装                |
| `runtime/server_runtime.*`                    | `runtime.md` / `threading_and_lifecycle.md`         | Server 生命周期与组装                |
| `runtime/audio_network_dispatcher.*`          | `server_audio_path.md`                              | queue → 编码 → broadcast             |
| `audio/devices/*_device_manager_factory.cpp`  | `factories.md`                                      | 平台设备后端选择                     |
| `audio/capture/audio_capture_factory.cpp`     | `factories.md`                                      | 平台采集后端选择                     |
| `audio/playback/audio_playback_factory.cpp`   | `factories.md`                                      | 平台回放后端选择                     |
| `c_api/aqua_capi.*`                           | `../include/aqua/c_api/aqua_capi.h`、`android_roadmap.md` | C API 与内部 IO/监督线程      |
| `c_api/android/jni/aqua_jni.cpp`              | `android_roadmap.md`                                | JNI 桥与诊断数组契约                 |

## 阅读顺序

### 新维护者

`README → architecture → audio_model → jitter_buffer → protocol → runtime → threading_and_lifecycle → 对应模块`

### 调试网络播放

`runtime → grpc → session → udp_transport → udp → jitter_buffer → diagnostics`

### 调试 Server 音频

`wasapi(capture) → capture_switching_design → packetizer → audio_frame_queue → server_audio_path → udp`

### 调试设备切换

`capture_switching_design / playback_switching_design → capture_manager / playback_manager → wasapi → runtime`

### 新增 Android backend

`playback → factories → devices → android_roadmap → C API/JNI 边界`
