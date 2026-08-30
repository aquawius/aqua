# Aqua Core / CLI 技术文档

本目录描述 **当前源码已经实现的系统**。阅读顺序建议：先看 `architecture.md`，再按数据流阅读 `modules/`
；遇到参数、协议或线程问题，再查对应专题文档。

## 文档规则

- 源码和测试是第一真相。文档不得描述尚未实现的功能。
- `aqua_core` 负责跨平台领域逻辑、网络协议、运行时编排和平台音频抽象；平台后端只实现抽象，不反向侵入 runtime。
- `aqua_app/cli` 是薄应用层：负责参数解析、进程生命周期和诊断展示，不复制 Core 业务逻辑。
- 单位必须写清楚：`frame`=一个 sample frame（所有声道一组采样）；`slot`=一个完整 `AudioFrame`；`byte`=原始 PCM
  字节。JitterBuffer 的容量单位是 slot，容量换算为 byte 时只用于内存统计。

## 模块地图

| 层                | 模块                                               | 文档                                                                                    |
|-------------------|----------------------------------------------------|-----------------------------------------------------------------------------------------|
| Runtime           | `ClientRuntime` / `ServerRuntime`                  | `modules/runtime.md`                                                                    |
| Audio model       | `AudioFormat` / `AudioBlock` / `AudioFrame`        | `modules/audio_model.md`                                                                |
| Playback          | `AudioPlayback` + WASAPI                           | `modules/playback.md`                                                                   |
| Capture           | `AudioCapture` + WASAPI                            | `modules/capture.md`                                                                    |
| Devices           | `AudioDeviceManager` + WASAPI                      | `modules/devices.md`                                                                    |
| Jitter            | `JitterBuffer`                                     | `modules/jitter_buffer.md`                                                              |
| Server handoff    | `AudioPacketizer` / `AudioFrameQueue` / Dispatcher | `modules/server_audio_path.md`, `modules/packetizer.md`, `modules/audio_frame_queue.md` |
| Session           | `SessionManager`                                   | `modules/session.md`                                                                    |
| gRPC              | Connect / Disconnect                               | `modules/grpc.md`                                                                       |
| UDP               | wire / transport / client / server                 | `modules/udp.md`                                                                        |
| Address           | IP parsing / formatting                            | `modules/address.md`                                                                    |
| Transport         | UDP socket / queue / strand                        | `modules/udp_transport.md`                                                              |
| Platform          | factories / WASAPI                                 | `modules/factories.md`, `modules/wasapi.md`                                             |
| Diagnostics       | logger / diagnostics                               | `modules/observability.md`                                                              |
| Protocol boundary | protobuf / format conversion                       | `modules/proto_boundary.md`, `modules/audio_format_converter.md`                        |

## 专题文档

- `architecture.md`：组件关系与端到端数据流
- `flow_model.md`：连接建立、稳态、故障与关闭的端到端时序
- `audio_design.md`：音频产品语义、格式、播放/采集原则
- `buffer_design.md`：JitterBuffer 算法细节、状态和边界
- `protocol.md`：wire 格式、session 握手、保活、失败语义
- `threading_and_lifecycle.md`：线程所有权、callback、stop/start 顺序
- `configuration_reference.md`：当前所有核心默认值与限制
- `testing.md`：测试分层和故障定位
- `build_and_release.md`：构建、preset、依赖和发布检查
- `operations_and_troubleshooting.md`：运行期排障
- `android_roadmap.md`：基于当前 Core 和旧 Android 已验证实现的 Android 路线
- `modules/source_map.md`：源码文件与模块文档的对照入口
