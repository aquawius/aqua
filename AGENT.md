# Aqua — Agent 工作指南

Aqua 是一个低延迟局域网网络音频共享系统。当前可用的音频实现是 Windows/WASAPI（采集 + 播放）与 Android/AAudio（仅播放）：
Server 负责采集与 UDP 广播，Client 负责 UDP 接收、JitterBuffer 与播放。两端的音频端点由 `CaptureManager` /
`PlaybackManager` 托管，设备故障时在会话内切换而不是终止进程。项目使用 C++23、CMake、vcpkg、Asio、gRPC、spdlog 和 GoogleTest。

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
    gRPC server / UDP server / capture / CaptureManager / packetizer / queue / dispatcher / ServerRuntime

aqua_client_core
    gRPC client / UDP client / JitterBuffer / playback / PlaybackManager / ClientRuntime

aqua_capi (AQUA_BUILD_C_API, 默认 OFF)
    ClientRuntime 的 C API 共享库（<build>/bin 下 libaqua.so / aqua.dll），
    含内部 IO/监督线程；Android 交叉编译供 app jniLibs 打包，Windows host
    仅用于 aqua_capi_test 冒烟。CLI 不使用它（直链静态 core 库）

aqua_server_cli
    Server 应用入口

aqua_client_cli
    Client 应用入口

aqua_client_gui (AQUA_BUILD_GUI，默认 ON)
    桌面图形客户端（aqua_app/gui，Dear ImGui + SDL3，直链 aqua_client_core）。
    imgui / sdl3 由 vcpkg manifest feature "gui" 提供；-DAQUA_BUILD_GUI=OFF 时
    不构建 target、不 find_package（同时关掉 preset 的 feature 则连依赖都不下载）
```

Server core 与 Client core 有意拆开：Server 只编译 capture 路径，Client 只编译 playback 路径；双方共享基础设施，但不把平台音频后端无意义地传播到另一端。

## 4. 绝对不能破坏的架构边界

### 4.1 控制面 / 数据面

```text
gRPC
    Connect / Disconnect / Keepalive

UDP
    Heartbeat / HeartbeatAck
    AudioFrame (RTP)
```

gRPC 不承载音频，也不是 UDP 的保活通道；heartbeat 保活属于 UDP/session 层。

### 4.2 Server 音频路径

```text
WASAPI Capture RT / MMCSS      ← CaptureManager 持有，故障时重建端点
    → AudioPacketizer::push
    → AudioFrameQueue (SPSC)
    → AudioNetworkDispatcher
    → UdpServer::broadcast
```

**Packetizer 没有独立线程。** `push()` 在 Capture realtime thread 中执行。不要为了局部功能再引入第二个
producer；如果需要补静音，应优先让 capture backend 生成与真实 `AudioBlock` 相同语义的输入（欠账驱动的时间轴补偿）。

切换事务（stop → join → start）由 `CaptureManager` 在控制线程执行：packetizer、queue、dispatcher、UDP 与 session 一律不重建。

### 4.3 Client 播放路径

```text
UDP receive
    → JitterBuffer::push
    → JitterBuffer::pull (playback RT)   ← PlaybackManager 持有后端，故障时重建流
    → WASAPI / AAudio Playback
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

控制面（gRPC）与数据面（UDP）**分别绑定**，而数据面是**逐族**的：

```text
rpc-ip                  控制面：gRPC 绑这一处。通配时 gRPC 自身双栈（两族客户端都能连上）
udp-ipv4 / udp-ipv6     数据面：v4 socket（AF_INET）与 v6 socket（AF_INET6 + v6_only=true）
                          共用 udp-port；取值 `none` = 关闭该族数据面
```

由此得到两条不变量（**完整清单 INV-1..INV-11 见 `aqua_core/doc/network_design.md` §7**，
代码注释里的 `INV-n` 引用的是那里）：

```text
INV-A  一个 socket 一族：v6 socket 保持 v6_only=true ⇒ ::ffff: 永不进入系统
INV-B  控制面可达族 ⊆ 数据面服务族：客户端按自己的族选通告槽，选不到就在 Connect 期失败
```

通知 Client 的 UDP endpoint **按客户端族分槽**下发：

```text
ConnectResponse.udp    给 IPv4 客户端的槽
ConnectResponse.udp6   给 IPv6 客户端的槽
```

每个槽有三个状态：

```text
具体地址          client 直接拨它（必须与本槽族同族）
通配 0.0.0.0/::   sentinel：client 改用自己连控制面时使用的地址
字段不存在        本 server 不服务该族数据面 ⇒ client 硬失败（不静默回落）
```

未显式指定 advertise 时，槽值跟随对应族的绑定地址；绑定为通配 ⇒ 下发通配 sentinel。
`--udp-advertise-ipv4` 必须是 v4、`--udp-advertise-ipv6` 必须是 v6（运行时不得跨族通告）：
族不匹配是**启动期**错误，不是运行期风险。

Client 默认使用 Server 下发的 endpoint；`--udp-force-port` 只在需要时覆盖 port。不要随意增加 force-ip，
让 Server advertised endpoint 与 Client 本地覆盖形成双重权威。

## 7. 音频格式与 MTU

- Server 一次运行固定 AudioFormat 与 `frame_count`。
- Client 通过 gRPC ConnectResponse 获取二者，不自行猜测。
- UDP Audio payload 必须是完整 AudioFrame，不能在 Aqua 协议层拆分成多个 datagram。
- UDP audio PCM payload budget 为 1400 bytes（IPv6 1500 MTU 推导 1440，再让 40B 隧道/封装余量）。
- `frame_count` 必须满足 `F × frame_bytes <= 1400`；auto-F 再与 5ms 包时长上限取小（`UDP_AUDIO_MAX_PACKET_MS`）。
- 不做隐式 resampling / transcoding。
- 设备可切换，格式不可变：切换后的流格式必须等于会话格式，否则该候选视为失败（不做转换、不重协商）。

设备切换的四条不变式（两侧通用）：

```text
Session alive        restart 不触碰 gRPC / UDP / session
Format immutable     restart 后格式必须一致
Endpoint replaceable 音频流生命周期独立于会话生命周期
Timeline continuous  允许 packet gap，禁止 seq 重置 / 时间轴重置 / 会话重建
```

## 8. CLI 语义

### Server

无需参数即可启动：

```text
rpc-ip             0.0.0.0
rpc-port              50051
udp-ipv4            0.0.0.0
udp-ipv6                 ::
udp-port              50000
capture               loopback
capture device         system default OUTPUT endpoint
```

默认即"两族数据面都能用"；要只服务一族就把另一族设为 `none`。

捕获源只有：

```text
--capture input
--capture loopback
```

其中 `loopback` 使用 OUTPUT endpoint；`input` 使用 INPUT endpoint。

`--capture-device-id` 可选：给出时按指定设备采集（路由 = PreferredDevice），省略时跟随该系统方向的系统默认（FollowSystem）。
运行期不提供修改采集目标的接口。

### Client

唯一必需参数：

```text
--rpc-ip
```

默认：

```text
rpc-port             50051
UDP                     从 gRPC 获取
playback device          system default OUTPUT
jb-capacity             30
name                     aqua-client
```

`--udp-force-port` 只覆盖 Server 下发的 UDP port，不覆盖 advertised IP。

`--playback-device-id` 指定播放设备（省略 = 跟随系统默认输出）。启动阶段若指定设备打不开，会沿降级链回退（且在诊断里留痕）；pin（钉住设备）与"设备回归自动切回"语义独立于本次降级，见 `aqua_core/doc/playback_switching_design.md` 第9节。

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
- runtime **format** hot switching（设备可切换，一次 Server 运行的格式与 F 固定）
- Server 侧运行期修改采集目标的接口
- STUN/TURN/ICE
- public-internet authentication/security protocol
- 第二个 playback RingBuffer
- 为一个局部问题创建第二套 runtime 或第二个 realtime producer
- 用静音 / 低能量等音频特征推断设备故障（只允许设备事件驱动切换）

## 13. 协作约定

- **不要执行 `git commit` / `git push` / 任何修改 git 状态的命令**（含建分支、update-ref、reset、checkout）。
  需要提交时，把改动文件清单与完整提交信息交给用户，由用户手动提交。只读命令（`status` / `log` / `diff`）不受限制。
- 文档与代码同步修改：改行为必须同时更新对应 `aqua_core/doc/` 文档；文档与代码冲突时以代码为准，并在文档中写清偏离原因。
- 不要一次改多个不相关模块；一个主题一次提交。
