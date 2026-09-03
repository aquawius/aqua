# Aqua Core / CLI 技术文档

本目录描述**当前源码已经实现的系统**。建议阅读顺序：先看 `architecture.md`，再按数据流阅读 `modules/`；遇到参数、协议或线程
问题，再查对应专题文档。

## 文档规则

- 源码和测试是第一真相。文档不得描述尚未实现的功能。
- `aqua_core` 负责跨平台领域逻辑、网络协议、运行时编排和平台音频抽象；平台后端只实现抽象，不反向侵入 runtime。
- `aqua_app/cli` 是薄应用层：负责参数解析、进程生命周期和诊断展示，不复制 Core 业务逻辑。
- 单位必须写清楚：`frame` = 一个 sample frame（所有声道一组采样）；`slot` = 一个完整 `AudioFrame`；`byte` = 原始 PCM
  字节。JitterBuffer 的容量单位是 slot，容量换算为 byte 时只用于内存统计。

## 模块地图

| 层              | 模块                                                 | 文档                                                                                     |
|-----------------|------------------------------------------------------|------------------------------------------------------------------------------------------|
| Runtime         | `ClientRuntime` / `ServerRuntime`                    | `modules/runtime.md`                                                                     |
| Audio model     | `AudioFormat` / `AudioBlock` / `AudioFrame` / `AudioError` | `modules/audio_model.md`                                                            |
| Capture         | `AudioCapture` + `CaptureManager` + WASAPI           | `modules/capture.md`、`capture_switching_design.md`                                        |
| Playback        | `AudioPlayback` + `PlaybackManager` + WASAPI + AAudio | `modules/playback.md`、`playback_switching_design.md`                                     |
| Devices         | `AudioDeviceManager` + WASAPI + AAudio               | `modules/devices.md`、`devices_and_format.md`                                             |
| Jitter          | `JitterBuffer`                                       | `modules/jitter_buffer.md`、`buffer_design.md`                                            |
| Server handoff  | `AudioPacketizer` / `AudioFrameQueue` / Dispatcher   | `modules/server_audio_path.md`、`modules/packetizer.md`、`modules/audio_frame_queue.md`   |
| Session         | `SessionManager`                                     | `modules/session.md`                                                                     |
| gRPC            | Connect / Disconnect                                 | `modules/grpc.md`                                                                        |
| UDP             | wire / transport / client / server                   | `modules/udp.md`、`modules/udp_transport.md`                                              |
| Transport       | UDP socket / queue / strand                          | `modules/udp_transport.md`                                                               |
| Platform        | factories / WASAPI / AAudio                          | `modules/factories.md`、`modules/wasapi.md`                                                |
| C API / JNI     | `aqua_capi` / Android JNI 桥                         | `../include/aqua/c_api/aqua_capi.h`、`android_roadmap.md`                                 |
| Diagnostics     | logger / diagnostics                                 | `modules/observability.md`、`diagnostics.md`                                              |
| Protocol boundary | protobuf / format conversion                       | `modules/proto_boundary.md`、`modules/audio_format_converter.md`                           |

## 专题文档

- `architecture.md`：组件关系与端到端数据流
- `flow_model.md`：连接建立、稳态、故障与关闭的端到端时序
- `audio_design.md`：音频产品语义、格式、播放/采集原则
- `buffer_design.md`：JitterBuffer 算法细节、状态和边界（模块 API 见 `modules/jitter_buffer.md`）
- `protocol.md`：wire 格式、session 握手、保活、失败语义
- `threading_and_lifecycle.md`：线程所有权、callback、stop/start 顺序
- `capture_switching_design.md`：Server 采集端点切换的设计决议（含 §14 实施修订记录）
- `playback_switching_design.md`：Client 回放端点切换的设计决议
- `devices_and_format.md`：设备值对象、路由与格式的关系
- `configuration_reference.md`：当前所有核心默认值与限制
- `testing.md`：测试分层、测试目标与故障定位
- `build_and_release.md`：构建、preset、依赖和发布检查（完整步骤见仓库根 `BUILD.md`）
- `operations_and_troubleshooting.md`：运行期排障（含设备切换）
- `design_decisions.md`：已冻结的设计决策（含设备切换相关的 D9/D12/D13 修订）
- `project_scope_and_requirements.md`：项目范围、非目标与产品不变量
- `security_and_deployment.md`：信任模型与部署限制
- `android_roadmap.md`：Android 分层、里程碑与验收标准（A0–A5 已完成，A6 播放设备切换已落地）
- `aaudio_backend_design.md`：AAudio 格式协商与设备路由的最终决议（§8 记录实施时的超范围项）
- `modules/source_map.md`：源码文件与模块文档的对照入口
