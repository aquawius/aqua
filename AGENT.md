# Aqua — Agent 工作指南

Aqua 是一个低延迟局域网网络音频共享系统。当前可用的音频实现是 Windows/WASAPI：Server 负责采集与 UDP 广播，Client 负责 UDP
接收、JitterBuffer 与播放。项目使用 C++23、CMake、vcpkg、Asio、gRPC、spdlog 和 GoogleTest。

本文件定义维护者、自动化 Agent 和后续开发者必须遵守的工作边界。详细设计以 `aqua_core/doc/` 为准。

## 1. 第一原则：源码、测试、Core 文档是唯一真相

- 先看当前源码和测试，再修改文档或实现。
- 如果旧 README、issue、旧分支或历史设计与当前源码冲突，以当前实现为准。
- 不要为了兼容旧设计主动恢复已经删除的 playback RingBuffer、旧 CLI 参数或旧 C API/Android 工程。
- 新行为必须有明确的实现位置、测试边界和文档语义。

## 2. 项目结构

```text
aqua/
├── CMakeLists.txt
├── CMakePresets.json
├── aqua_core/
│   ├── include/aqua/          # Core 公共头（compat/ 为跨 STL 兼容层）
│   ├── src/                   # Core 实现（c_api/ 为 C API 共享库实现）
│   ├── proto/                 # gRPC / protobuf schema
│   ├── tests/                 # GoogleTest
│   └── doc/                   # Core 技术文档
└── aqua_app/
    ├── aqua_android/            # Android Gradle 工程（JNI 走 core 的 aqua_capi）
    │   ├── app/                 # Compose UI / AquaService / jniLibs 产物
    │   └── build_android.ps1    # native 交叉编译 + strip + jniLibs 同步
    └── cli/
        ├── cli_parser/          # Server / Client 参数解析
        └── doc/                 # CLI 文档
```

## 3. CMake targets

```text
aqua_proto
    protobuf / gRPC 生成代码与 AudioFormat proto 边界转换

aqua_core_base
    logger / diagnostics / session / address / UDP transport / device manager

aqua_server_core
    gRPC server / UDP server / capture / packetizer / queue / dispatcher / ServerRuntime

aqua_client_core
    gRPC client / UDP client / JitterBuffer / playback / ClientRuntime

aqua_capi (AQUA_BUILD_C_API, 默认 OFF)
    ClientRuntime 的 C API 共享库（<build>/bin 下 libaqua.so / aqua.dll），
    含内部 IO/监督线程；Android 交叉编译供 app jniLibs 打包，Windows host
    仅用于 aqua_capi_test 冒烟。CLI 不使用它（直链静态 core 库）

aqua_server_cli
    Server 应用入口

aqua_client_cli
    Client 应用入口
```

Server core 与 Client core 有意拆开：Server 只编译 capture 路径，Client 只编译 playback 路径；双方共享基础设施，但不把平台音频后端无意义地传播到另一端。

## 4. 绝对不能破坏的架构边界

### 4.1 控制面 / 数据面

```text
gRPC
    Connect / Disconnect

UDP
    HELLO / HELLO_ACK
    AudioFrame
```

gRPC 不承载音频，也不是 UDP 的保活通道；HELLO 保活属于 UDP/session 层。

### 4.2 Server 音频路径

```text
WASAPI Capture RT / MMCSS
    → AudioPacketizer::push
    → AudioFrameQueue (SPSC)
    → AudioNetworkDispatcher
    → UdpServer::broadcast
```

**Packetizer 没有独立线程。** `push()` 在 Capture realtime thread 中执行。不要为了局部功能再引入第二个
producer；如果需要补静音，应优先让 capture backend 生成与真实 `AudioBlock` 相同语义的输入。

### 4.3 Client 播放路径

```text
UDP receive
    → JitterBuffer::push
    → JitterBuffer::pull (playback RT)
    → WASAPI Playback
```

当前 Client 只有一个应用层 playback buffer：JitterBuffer。不要重新引入独立 playback RingBuffer 与第二套水位控制。

### 4.4 实时路径

RT 路径禁止：

- mutex / blocking lock
- 动态内存分配
- 同步文件/网络 I/O
- 睡眠等待业务条件
- 依赖另一个 realtime producer 的共享可变状态

开发诊断日志可以临时打开，但同步 RT log 不能作为生产性能基线；Release preset 默认关闭 JitterBuffer RT debug logging。

## 5. JitterBuffer 不变量

- 容量单位是 slot；byte 只用于存储几何与统计。
- `lead` 是 sequence timeline 的领先量，不等于真实 occupied slot 数。
- Warning FILL 不直接写 water；它通过重播 READY slot 减慢 playback timeline。
- Warning DROP 跳过完整 slot，使 playback timeline 加速。
- Warning correction 从 1 slot 起逐步增长并受 `max_step` 限制。
- Deadline-high 是一次性 hard correction。
- Reanchor 是时间轴异常时的强恢复机制。
- 缺帧不得阻塞 playback RT；输出静音并继续推进时间轴。
- Warning Fill 的 correction unit 是 slot，不依赖 WASAPI playback callback 的批大小；一个 callback 可以跨越 slot 或只覆盖一个
  slot 的一部分。

详见 `aqua_core/doc/buffer_design.md` 与 `aqua_core/doc/modules/jitter_buffer.md`。

## 6. 网络与地址语义

Server 只有一个本地绑定地址：

```text
server-ip
    ├─ gRPC bind
    └─ UDP bind
```

通知 Client 的 UDP endpoint 独立：

```text
advertise-ip
advertise-udp-port
```

未显式指定时，advertised IP/port 分别跟随 server-ip/udp-port。Client 默认使用 Server 下发的 endpoint；`--force-udp-port`
只在需要时覆盖 port。不要随意增加 force-ip，让 Server advertised endpoint 与 Client 本地覆盖形成双重权威。

## 7. 音频格式与 MTU

- Server 一次运行固定 AudioFormat 与 `frame_count`。
- Client 通过 gRPC ConnectResponse 获取二者，不自行猜测。
- UDP Audio payload 必须是完整 AudioFrame，不能在 Aqua 协议层拆分成多个 datagram。
- UDP audio PCM payload budget 为 1443 bytes，按 IPv6 1500 MTU 计算。
- `frame_count` 必须满足 `F × frame_bytes <= 1443`。
- 不做隐式 resampling / transcoding。

## 8. CLI 语义

### Server

无需参数即可启动：

```text
server-ip             0.0.0.0
rpc-port              50051
udp-port              50000
capture               loopback
capture device         system default OUTPUT endpoint
```

捕获源只有：

```text
--capture input
--capture loopback
```

其中 `loopback` 使用 OUTPUT endpoint；`input` 使用 INPUT endpoint。

### Client

唯一必需参数：

```text
--server-ip
```

默认：

```text
server-rpc             50051
UDP                     从 gRPC 获取
playback device          system default OUTPUT
jitter-slots             90
name                     aqua-client
```

`--force-udp-port` 只覆盖 Server 下发的 UDP port，不覆盖 advertised IP。

## 9. 修改代码时的工作顺序

1. 先查 `aqua_core/doc/` 对当前模块的设计约束。
2. 查对应 `.h/.cpp` 的真实调用关系。
3. 先补/更新领域或边界测试，再修改实现。
4. 修改后检查线程所有权、异常路径、stop/start 生命周期以及 RT 契约。
5. 再更新顶层 README / CLI 文档；顶层文档不应定义与 parser/Core 不同的第二套配置语义。

## 10. 构建与验证

标准 Windows Debug：

```powershell
cmake --preset windows-x64-debug
cmake --build cmake_build/windows-x64-debug --config Debug
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug --output-on-failure
```

Release：

```powershell
cmake --preset windows-x64-release
cmake --build cmake_build/windows-x64-release --config Release
```

发布/冻结前至少验证：

- build 无 error；
- CTest 全绿；
- `--help` / `--list-devices` 与 parser 一致；
- Server/Client 默认值一致且与 `configuration_reference.md` 一致；
- IPv4/IPv6 地址语义未回归；
- Release 默认关闭 RT debug logging；
- 实机验证 loopback、input、playback 设备方向；
- JitterBuffer warning/deadline/reanchor 路径没有新增 RT 竞争。

## 11. 推荐阅读顺序

```text
aqua_core/doc/README.md
aqua_core/doc/architecture.md
aqua_core/doc/flow_model.md
aqua_core/doc/audio_design.md
aqua_core/doc/buffer_design.md
aqua_core/doc/protocol.md
aqua_core/doc/threading_and_lifecycle.md
aqua_core/doc/configuration_reference.md
```

模块级问题再进入：

```text
aqua_core/doc/modules/source_map.md
```

## 12. 当前明确非目标

- codec / compression
- automatic resampling / transcoding
- WASAPI Exclusive
- runtime device/format hot switching
- STUN/TURN/ICE
- public-internet authentication/security protocol
- 第二个 playback RingBuffer
- 为一个局部问题创建第二套 runtime 或第二个 realtime producer
