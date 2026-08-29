# Aqua Design Documentation

这些文档描述当前源码实现的设计契约，而不是愿望清单。若旧讨论、旧设计笔记与这里冲突，以当前源码、测试和本目录的最新文档为准。

## 文档地图

| 文档 | 内容 | 性质 |
|---|---|---|
| `project_scope_and_requirements.md` | 项目目标、范围、非目标、功能/实时性要求 | 需求基线 |
| `architecture.md` | 系统分层、模块边界、server/client 拓扑 | 架构基线 |
| `audio_design.md` | 音频数据模型、设备、格式、capture/playback | 音频设计 |
| `buffer_design.md` | JitterBuffer/SPSC/水位/reanchor | 核心算法冻结 |
| `protocol.md` | gRPC + UDP wire/session 协议 | 协议基线 |
| `threading_and_lifecycle.md` | 线程所有权、锁、callback、start/stop | 并发契约 |
| `devices_and_format.md` | 设备枚举/选择、loopback、格式决策 | 功能设计 |
| `diagnostics.md` | 指标、日志、RT debug 开关 | 可观测性 |
| `testing.md` | 测试层级、回归重点、验证矩阵 | QA 基线 |
| `build_and_release.md` | CMake preset、依赖、发布检查 | 工程基线 |
| `security_and_deployment.md` | trust model、地址、session 风险、部署建议 | 安全基线 |
| `design_decisions.md` | 已冻结的重要决策及拒绝过的方向 | ADR 集合 |
| `configuration_reference.md` | Server/Client 全部 CLI 与 Runtime 配置契约 | 配置基线 |
| `operations_and_troubleshooting.md` | 运行、故障排查和诊断路径 | 运维基线 |

## 当前冻结原则

1. Client 播放侧只有一个应用层缓冲：JitterBuffer。
2. Server 的 capture→network 交接队列不属于 playback buffering model。
3. Audio domain 不依赖 Network domain，UDP 层不认识 `AudioFrame`。
4. Runtime 是 composition root，backend thread 不负责上层生命周期管理。
5. RT callback 不允许锁、堆分配或同步 I/O；JitterBuffer RT debug 日志是明确的、默认关闭的例外诊断开关。
6. Server bind address 与 advertised address 是两个完全不同的概念；wildcard bind 是合法的，wildcard advertised address 则触发 client fallback。
7. Server 默认音频格式来自 capture backend；显式格式属于用户 override。
