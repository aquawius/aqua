# Aqua 构建指南

本文档描述当前仓库的真实构建方式。 **当前实际音频后端为 Windows/WASAPI；Linux/macOS/Android 的 preset
是工程骨架或未来扩展入口，不等于音频后端已经实现。**

---

## 1. 前置环境

| 工具 / 组件 | 要求                                      |
|-------------|-------------------------------------------|
| C++         | C++23                                     |
| CMake       | 4.2+                                      |
| vcpkg       | manifest 模式，`VCPKG_ROOT` 必须可用      |
| Windows     | Visual Studio 2026                        |
| 依赖        | Asio / gRPC / protobuf / spdlog / cxxopts |
| 测试        | GoogleTest / CTest                        |

依赖清单位于：

```text
vcpkg.json
```

示例：

```powershell
$env:VCPKG_ROOT = "C:\CodingDeps\vcpkg"
```

vcpkg 依赖按默认目录缓存（manifest 模式下为 `<build>/vcpkg_installed/<triplet>`）， 无需在 preset 中指定
`VCPKG_INSTALLED_DIR` 或 `VCPKG_MANIFEST_DIR`。

---

## 2. 工程布局

```text
aqua/
├── CMakeLists.txt
├── CMakePresets.json
├── aqua_core/
│   ├── CMakeLists.txt
│   ├── include/aqua/
│   ├── src/
│   ├── proto/
│   ├── tests/
│   └── doc/
└── aqua_app/
    └── cli/
```

---

## 3. CMake targets

Core：

```text
aqua_proto
    protobuf / gRPC generated code + format conversion

aqua_core_base
    logger / diagnostics / session / address / UDP transport / device manager

aqua_server_core
    Server runtime + capture + packetizer + queue + dispatcher + gRPC/UDP server

aqua_client_core
    Client runtime + JitterBuffer + playback + gRPC/UDP client
```

CLI：

```text
aqua_server_cli
aqua_client_cli
```

测试目标：

```text
aqua_tests
```

---

## 4. Windows Debug

配置：

```powershell
cmake --preset windows-x64-debug
```

构建：

```powershell
cmake --build cmake_build/windows-x64-debug --config Debug
```

测试：

```powershell
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug --output-on-failure
```

Debug preset 会启用：

```text
AQUA_DEBUG=ON
AQUA_JITTER_BUFFER_RT_DEBUG_LOG=ON
```

后者用于开发时观察 JitterBuffer RT 路径；不要用开启 RT 同步日志的结果作为正式性能基线。

---

## 5. Windows Release

配置：

```powershell
cmake --preset windows-x64-release
```

构建：

```powershell
cmake --build cmake_build/windows-x64-release --config Release
```

Release preset 默认：

```text
AQUA_DEBUG=OFF
AQUA_JITTER_BUFFER_RT_DEBUG_LOG=OFF
```

生产 Release 不会默认启用 JitterBuffer 同步 RT debug logging。

---

## 6. 运行

### Server

Server 无参数即可启动：

```powershell
.\aqua_server_cli.exe
```

默认：

```text
server-ip             0.0.0.0
rpc-port              50051
udp-port              50000
capture               loopback
capture device         系统默认 OUTPUT endpoint
```

### Client

Client 最少只需要 Server IP：

```powershell
.\aqua_client_cli.exe --server-ip 192.168.1.10
```

默认：

```text
server-rpc             50051
UDP                     从 gRPC ConnectResponse 获取
playback device          系统默认 OUTPUT endpoint
jitter-slots             30
name                     aqua-client
```

Client 可选：

```powershell
.\aqua_client_cli.exe --server-ip 192.168.1.10 --force-udp-port 52000
```

此选项只覆盖 Server 下发的 UDP port，不覆盖 Server advertised IP。

---

## 7. Server 网络参数语义

Server 本地只绑定一个 IP：

```text
--server-ip
    同时用于 gRPC bind 和 UDP bind
    默认 0.0.0.0
```

本地端口：

```text
--rpc-port
    默认 50051

--udp-port
    默认 50000
```

通知 Client 的 UDP endpoint 独立：

```text
--advertise-ip
--advertise-udp-port
```

未指定时：

```text
advertise-ip       = server-ip
advertise-udp-port = udp-port
```

例如：

```text
本地监听：
    0.0.0.0:50000

通知 Client：
    203.0.113.10:52000
```

这种配置适用于多网卡、NAT、容器、端口映射等部署场景。

---

## 8. 音频格式与 frames-per-slot

Server 的：

```text
--encoding
--channels
--sample-rate
```

要么全部省略，要么全部指定。

全部省略时：

```text
capture backend → 读取目标设备默认共享模式格式
```

显式格式必须有效。

`--frames-per-slot`：

```text
0       → 按 MTU 自动推导
>=16    → 显式指定，但必须满足 F × frame_bytes <= 1443
```

当前 UDP audio payload budget：

```text
1443 bytes
```

该预算按 IPv6 1500-byte MTU 计算：

```text
1500 - 40(IPv6) - 8(UDP) - 9(Aqua audio header) = 1443
```

例如：

```text
2ch F32:
    frame_bytes = 8
    F = floor(1443 / 8) = 180

1ch F32:
    frame_bytes = 4
    F = floor(1443 / 4) = 360
```

---

## 9. Capture source

Server 当前只有两种 capture source：

```text
--capture input
    INPUT endpoint，例如麦克风

--capture loopback
    OUTPUT endpoint 的 WASAPI loopback，例如扬声器、耳机、数字输出
```

`--device-id` 必须与 source 的 endpoint direction 匹配。

设备不指定时，使用对应方向的系统默认设备。

### Loopback quiescence fallback

Windows loopback 在某些设备/驱动场景下，当所有 render client 退出后可能进入 quiescence，使 capture event 暂时完全停止。

当前 Core 的处理方式是：

```text
WASAPI event-driven
      ↓
20ms bounded wait
      ↓
timeout + GetNextPacketSize()==0
      ↓
synthetic silence AudioBlock
      ↓
原有 callback → Packetizer → Queue → Dispatcher → UDP
```

该设计保持 Packetizer 只有一个 producer，并让下游继续获得连续的音频时间轴。

---

## 10. JitterBuffer

默认容量：

```text
30 slots
```

默认阈值：

```text
warning_low  = 30%
normal_low   = 45%
target        = 60%
normal_high   = 75%
warning_high  = 90%
```

Warning Fill 是软时间轴校正：重播 READY slot，使 playback timeline 暂时减速；Warning Drop 是跳过完整 slot，使时间轴加速。

默认 step：

```text
1,1,1,1,2,2,2,2,3,3,...
```

并受 `max_step` 限制。Deadline-high 和 reanchor 是更强的恢复路径。

---

## 11. 测试

标准完整回归：

```powershell
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug --output-on-failure
```

测试重点：

```text
Audio model
Packetizer / Queue
JitterBuffer
UDP / Session
Runtime
WASAPI backend
```

JitterBuffer 特别需要回归：

```text
pre-roll boundary
late / missing / duplicate
slot collision
cross-slot playback
warning Fill replay
warning Drop
warning step growth
Deadline correction
reanchor
reanchor stale-slot cleanup
```

---

## 12. 版本

当前版本单一来源：

```cmake
# 根 CMakeLists.txt
set(AQUA_VERSION "0.2.0")
```

由它派生：

```text
AQUA_CORE_VERSION
AQUA_SERVER_CLI_VERSION
AQUA_CLIENT_CLI_VERSION
AQUA_CLIENT_ANDROID_VERSION
```

`vcpkg.json` 的 `version` 是纯字面量，无法引用 CMake 变量；升级版本时需要手动保持同步。

---

## 13. 常见构建问题

### `permission denied` / buildtree 文件占用

通常是 IDE、测试进程或仍在运行的 Aqua 可执行文件占用了文件。

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'aqua*' }
```

必要时：

```powershell
Stop-Process -Name aqua_server_cli,aqua_client_cli -Force
```

然后重新 configure/build。

### CMake 找不到 vcpkg

确认：

```powershell
echo $env:VCPKG_ROOT
```

并确认：

```text
%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
```

存在。

---

## 14. 构建与文档关系

构建参数的权威来源：

```text
CMakeLists.txt
CMakePresets.json
vcpkg.json
```

Core 行为的权威说明：

```text
aqua_core/doc/
```

CLI 参数的权威实现：

```text
aqua_app/cli/cli_parser/
```

顶层 README 用于项目介绍和快速上手，不应重新定义一套独立的配置语义。
