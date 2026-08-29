# Design Decisions / ADR Summary

## ADR-001：Client 只保留一个应用层播放 Buffer

采用单 JitterBuffer；不再引入独立 RingBuffer/WatermarkController。WASAPI callback 的输出尺寸差异通过 JB slot 内 `read_offset` 解决。

## ADR-002：Buffer 容量以 bytes 可观测，以 slots 控制

JB 同时暴露 capacity/used bytes；水位与校正仍以完整 AudioFrame slot 为控制单位。

## ADR-003：丢包在 playback deadline 决策

Producer 不负责“判断最终丢失”；consumer 到达 play_seq 时才把缺失 slot解释为 silence。

## ADR-004：Server/Client 音频格式 session 内固定

Server 一个运行期采用一个有效 AudioFormat + F；所有 session 共享。Client 必须按 ConnectResponse 创建 JB/playback。

## ADR-005：Server 默认格式由 backend

不再硬编码 F32/48 kHz 作为默认音频流格式。backend default 成为默认值；显式配置只作为 override。

## ADR-005a：设备在 Runtime 启动配置中冻结

Server 在构造阶段将 capture source + device 请求解析为一个具体 backend device ID，并以同一设备完成默认格式探测和实际 capture startup。运行期间不自动跟随系统默认设备变化；换设备必须 stop 后重新启动。

## ADR-006：wildcard bind 与 advertise 解耦

`0.0.0.0/::` 可以作为本地 bind address，也可以作为 advertised sentinel。Server 不要求 advertised address 必须是具体地址；Client 发现 wildcard，或收到空/非法 advertised value 时，使用其 gRPC `server_ip` fallback。

## ADR-007：设备选择采用 discover + explicit id

CLI 通过 `--list-devices` 发现设备，通过 `--device-id` 选择；不采用不可复用的交互式编号作为持久配置。

## ADR-008：RT logging 是显式编译开关

正常 RT 路径绝不同步日志；问题排查时才允许通过 `AQUA_JITTER_BUFFER_RT_DEBUG_LOG` 打开。

## ADR-009：Runtime start/stop 内部串行化

lifecycle mutex 保证 startup/teardown 不交叠，允许 control threads 安全并发调用 stop。

## ADR-010：UDP config plane 与 data plane 分离

配置操作由 mutex 保护；异步 socket 运行仍由 strand 串行化。


## 8. 最小启动路径

Server 的产品默认必须能够以 `aqua_server` 直接启动。默认是 `0.0.0.0:50051` + `0.0.0.0:9999`、系统默认 OUTPUT endpoint 的 loopback，以及该 endpoint 的 backend default/shared-mode audio format。用户可以仅增加 `--device-id <OUTPUT_ID>` 切换 loopback 设备。

Client 的正常启动只要求 `--server-ip <IP> --server-rpc <PORT>`；播放设备使用系统默认 OUTPUT，音频格式完全由 Server 的 ConnectResponse 决定。

格式不能通过静态配置文件预先“猜定”：Server 必须先完成 device resolution 和 backend default-format query，然后才能确定 frame geometry。只有显式提供完整 `encoding + channels + sample-rate` 三元组时才覆盖 backend default。
