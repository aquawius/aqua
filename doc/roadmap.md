# 开发路线与实现状态

## 1. Milestones

### M0 工程基础
CMake + C++23 + vcpkg + spdlog + GoogleTest + Asio + 基础 UDP Transport。

### M1 Windows PCM
WASAPI Loopback 采集 + PCM + UDP Unicast + WASAPI 播放（Windows ↔ Windows 最小链路）。

### M2 SessionManager
`uint32_t session_id` + 创建/删除 + endpoint + `last_seen` + timeout + UDP HELLO / HELLO_ACK。

### M3 gRPC + NAT
gRPC Server/Client + Connect/Disconnect + 固定 UDP media port + NAT endpoint 自动发现 + UDP HELLO 单路保活。

### M4 Stable PCM Playout
JitterBuffer：playout deadline、乱序重排、去重、late packet、静音填充；固定 target latency 30ms；`JitterBuffer → RingBuffer → WASAPI`。

### M5 Diagnostics & Buffer Policy
DiagnosticsManager：RTT / interarrival jitter / loss（lost+late+dup）/ JB·RB 水位 / underrun / occupancy slope / estimated playback-rate drift；`--jitter-buffer` 手动调整 target；周期诊断日志。

### M5+ Clock Correction Research
根据 M5 实测 drift 数据决定 correction（sample slip / time-scale / 平台时钟 / 无需 correction），**不预设答案，不引入 resampler**。

### M6 跨平台
PipeWire（Linux）、AAudio（Android）、Qt6 桌面、Kotlin+JNI Android。

### M7 Opus
PCM 链路稳定后再加 Opus / Codec 抽象 / bitrate / frame duration / PLC / FEC。

## 2. 当前明确不做的事情

- UUID / session token / 用户系统 / 账号系统 / TLS 自定义认证
- STUN / TURN / ICE / 双方 NAT / 对称 NAT
- Codec 协商 / Server 端重采样 / Server 端混音
- Multicast / 多人房间 / 云端服务 / 复杂 RPC
- 每 Session 一个 UDP port

保持系统简单：gRPC 只 Connect/Disconnect，UDP 只 HELLO / HELLO_ACK / AUDIO。

## 3. 实现状态

**M0–M5 已完成，M6 进行中（core 运行时重构 + C API + Android 构建链/App 已完成）。**

### 已完成要点

- **M0–M3**：logger / SessionManager（状态机 + 线程安全）/ audio_format + converter / proto（Connect/Disconnect）/ CLI / UDP Transport / SPSC RingBuffer / packet 编解码 / WASAPI 采集播放 / gRPC server+client / NAT 单路保活。
- **M4**：JitterBuffer（playout deadline、预分配 storage、回绕、异常注入测试）。
- **M5**：DiagnosticsManager（RTT/jitter/loss/occupancy/underrun/drift slope）+ `--jitter-buffer` + 周期日志。
- **M6（进行中）**：
  - core 运行时重构：编排逻辑从 CLI main 下沉到 `ServerRuntime`/`ClientRuntime`（pImpl + start/run/shutdown + 回调）。
  - C API（`include/aqua.h` + `aqua_capi.cpp`）：不透明句柄 + 回调 + 状态码 + 音频格式；`aqua_client_get_diagnostics` / `aqua_client_get_audio_format`。
  - Android 构建链 + AAudio 回放后端：`android-arm64-{debug,release}` preset；`aqua_capi` 编成 `libaqua.so`；`AQUA_API` 导出宏。
  - Android App（Kotlin/Compose + JNI）：连接/高级参数/诊断/设置 UI；前台 `AquaService`（MediaSession + MediaStyle 通知 + 音频焦点 + 播放/停止）；通知权限与设置入口；`assembleRelease`。
  - 版本号分层：`version.h.in`（core）/ `cli_version.h.in`（CLI）/ Gradle 直读 CMake（Android）。

### 待办

- PipeWire 后端（Linux）/ AAudio 采集（Android mic）。
- Qt6 桌面 UI。
- M5+ clock correction（待实测数据）。

## 4. 已知偏差与遗留

- **无 client 端格式转换**：当前 client 直接用 server 返回格式播放，设备不支持时需后续实现（见 [modules.md §5](modules.md#5-client-audio-format-conversion)）。
- **sample_position 截断**：`uint32_t` 在 48kHz 下约 24.8h 回绕；后续协议版本可改 `uint64_t`。
- **断连重连**：`--auto-reconnect`（默认关）启用指数退避；未启用时保持退出行为。
