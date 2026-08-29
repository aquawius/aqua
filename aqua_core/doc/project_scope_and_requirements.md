# Project Scope & Requirements

## 1. 目标

Aqua 的目标是让一台机器的系统/输入音频以较低端到端延迟共享给一个或多个 Client，并在网络抖动、乱序和丢包条件下保持连续播放。

## 2. 核心功能

产品级 CLI 最小路径：Server 零参数启动；Client 只要求 `--server-ip` 与 `--server-rpc`。

### Server

- 监听 gRPC 控制面与 UDP 数据面。
- 查询 active audio devices。
- 选择输入设备或输出设备 loopback。
- 未显式指定格式时采用 capture backend 的 shared-mode 默认格式。
- 将采集的 PCM 重切为固定 `AudioFrame`。
- 为每个 session 广播相同编码后的 UDP datagram。

### Client

- 通过 gRPC 创建 session。
- 获取 UDP endpoint、AudioFormat、`frame_count`。
- wildcard UDP advertisement (`0.0.0.0` / `::`) 时使用 `--server-ip` fallback。
- 查询并选择 output device；未指定时使用系统默认设备。
- 通过 JitterBuffer 处理乱序、丢包、startup pre-roll、Fill/Drop。
- 监控 HELLO_ACK liveness。

## 3. 非目标

当前版本不把以下能力视为已实现目标：

- 公网不可信环境下的身份认证与加密。
- 多 server 混音。
- Client 侧多路本地混音。
- 自动重采样/自动声道矩阵转换。
- Linux/macOS/Android 的完整音频 backend。

## 4. 实时性要求

### RT path

必须避免：

- mutex / blocking lock
- heap allocation
- synchronous logging / console I/O
- blocking system calls
- executor submission

允许：

- bounded memcpy
- atomic relaxed/acquire/release 操作
- 已构造的无状态策略函数

### 网络

允许 UDP 丢包；协议不能依赖可靠传输语义。

### 故障策略

- 丢帧：Client 播放静音并继续推进播放时间线。
- 网络超时：Client 进入 `Degraded`。
- capture/playback backend 错误：Runtime 进入 `Degraded` 并由 control path 终止当前进程。
- 配置错误：启动前失败，不进入 Running。


## 5. 格式参数原则

除非用户明确指定 `--encoding`、`--channels`、`--sample-rate` 三项，否则 Server 不人为调整音频格式；实际格式由所选 capture endpoint 的 backend default/shared-mode stream 决定。显式格式如果 backend 不原生支持则启动失败，不进行隐式转换。
