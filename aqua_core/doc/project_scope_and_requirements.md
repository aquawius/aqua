# 项目范围与要求

## 目标

Aqua 是低延迟局域网音频共享系统：Server 从本机音频 endpoint 采集 PCM，切成定长帧后 UDP 广播；Client 通过 gRPC 建立
session，通过 UDP 接收音频，经 JitterBuffer 重排后播放。

## 当前功能

**平台后端**

- Windows / WASAPI：loopback 采集与回放
- Android / AAudio：回放（含最小设备管理器）
- Linux / macOS：可编译，无音频后端

**传输与控制面**

- gRPC `Connect` / `Disconnect`
- UDP Audio / HELLO / HELLO_ACK
- session 超时回收与 NAT endpoint 学习
- IPv4 / IPv6 literal 地址处理与 wildcard 通告回退

**音频核心**

- JitterBuffer：pre-roll、Fill、Drop、缺帧静音、reanchor
- loopback quiescence 的欠账驱动时间轴补偿（合成静音）
- capture 运行态诊断（Active / Silent / Starved 与相关计数）

**设备切换（capture 与 playback 各自）**

- 设备故障按候选链重建端点，会话、格式与时间线不变
- 跟随系统默认设备变化；指定设备时保留用户意图并支持自动切回
- 10s / 3 次的重试预算防插拔风暴

**应用**

- Windows CLI：`aqua_server_cli` / `aqua_client_cli`（含设备枚举与运行诊断）
- Android App：Kotlin/Compose，经 C API + JNI 驱动 `ClientRuntime`
- C API（`aqua_capi`）：跨平台，供非 C++ 前端复用 client 能力

## 当前非目标

- 音频压缩 codec（当前为裸 PCM）
- NAT traversal
- 公网安全协议（无认证、无加密）
- Windows Exclusive / Android Exclusive
- 多级播放缓冲（只有 JitterBuffer 一层）
- **运行期切换音频格式或 F**（设备可切换，格式不可变）
- 客户端隐式 resampling / 格式转换
- Android capture / loopback（`OUTPUT_LOOPBACK` 在 Android 返回 `NotSupported`）
- Server 侧的运行时手动切换入口（切换目标即 CLI 配置，不提供运行中改目标的接口）

## 产品不变量

1. Server 一次运行固定格式与 F。
2. Client 必须按 Server 格式创建播放链路。
3. 一个 AudioFrame 对应一个完整 UDP Audio datagram。
4. playback callback 必须始终得到可播放输出，缺数据以静音补齐。
5. RT 路径不允许阻塞 / 分配 / 同步 I/O。
6. 设备切换不改变会话、格式与时间线（允许 packet gap，禁止 seq 重置）。
