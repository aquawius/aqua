# Aqua 构建指南

本文档描述当前仓库的真实构建方式。 **实际音频后端：Windows/WASAPI（采集 + 播放）与 Android/AAudio（播放）。
Linux/macOS 的 preset 是工程骨架，不代表对应平台音频后端已经实现。**

---

## 1. 前置环境

| 工具 / 组件 | 要求                                          |
|-------------|-----------------------------------------------|
| C++         | C++23                                         |
| CMake       | 4.2+                                          |
| vcpkg       | manifest 模式，`VCPKG_ROOT` 必须可用          |
| Windows     | Visual Studio 2026                            |
| Android     | NDK（`ANDROID_NDK_HOME`）+ JDK / Android SDK |
| 依赖        | Asio / gRPC / protobuf / spdlog / cxxopts     |
| 测试        | GoogleTest / CTest                            |

Android 构建补充：

```text
NDK 版本      不硬编码；脚本按 ANDROID_NDK_HOME 自动探测
ABI           arm64-v8a
minSdk        28
STL           c++_shared（libc++_shared.so 随 APK 打包）
vcpkg triplet arm64-android（首次 configure 自动安装）
```

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
    ├── aqua_android/
    │   ├── app/                # Compose / Service / jniLibs 产物
    │   └── build_android.ps1   # native 交叉编译 + strip + jniLibs 同步
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
    Server runtime + capture + CaptureManager + packetizer + queue + dispatcher + gRPC/UDP server

aqua_client_core
    Client runtime + JitterBuffer + playback + PlaybackManager + gRPC/UDP client

aqua_capi (AQUA_BUILD_C_API，默认 OFF；Android preset 强制 ON)
    ClientRuntime 的稳定 C API 共享库，产物统一命名为 aqua（libaqua.so / aqua.dll），
    输出到 <build>/bin。Android 交叉编译时含 JNI 动态注册并静态链入 gRPC/protobuf/abseil，
    供 app jniLibs 打包；Windows host 构建仅用于 aqua_capi_test 冒烟。CLI 不使用它（直链静态 core 库）。
```

CLI：

```text
aqua_server_cli
aqua_client_cli
```

测试目标（按模块拆分，全部经 `gtest_discover_tests` 注册，故可按用例名过滤）：

| 目标                              | 内容                                    | 平台       |
|-----------------------------------|-----------------------------------------|------------|
| `aqua_tests`                      | logger                                  | 全         |
| `aqua_diagnostics_tests`          | diagnostics                             | 全         |
| `aqua_net_tests`                  | gRPC / session / UDP / 格式转换          | 全         |
| `aqua_audio_tests`                | AudioFormat / AudioFrameQueue            | 全         |
| `aqua_audio_packetizer_tests`     | packetizer                              | 全         |
| `aqua_jitter_buffer_tests`        | JitterBuffer（含边界与回归）             | 全         |
| `aqua_playback_manager_tests`     | PlaybackManager 切换事务                 | 全         |
| `aqua_capture_manager_tests`      | CaptureManager 切换事务                 | 全         |
| `aqua_capi_test`                  | C API（需 `AQUA_BUILD_C_API=ON`）        | 全         |
| `aqua_wasapi_device_manager_tests`| WASAPI 设备解析                         | 仅 Windows |
| `aqua_wasapi_capture_tests`       | WASAPI 采集                             | 仅 Windows |
| `aqua_wasapi_playback_tests`      | WASAPI 回放                             | 仅 Windows |

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

## 6. Android（AAudio playback）

构建分两层，**不要把 CMake 交叉编译失败与 Compose/JNI bug 混进同一次调试循环**：先保证 native 库独立构建成功，
再进入 Gradle 打包。

### 6.1 native 库（libaqua.so）

在仓库根目录执行（需要 `ANDROID_NDK_HOME` 与 `VCPKG_ROOT`）：

```powershell
powershell -ExecutionPolicy Bypass -File aqua_app/aqua_android/build_android.ps1
```

可选参数：

```text
-SkipDebug     只构建 release 库
-SkipRelease   只构建 debug 库
```

脚本内部流程：

```text
cmake --preset android-arm64-{debug,release}     # vcpkg arm64-android 依赖首次自动安装
cmake --build cmake_build/<preset> --target aqua_capi
llvm-strip --strip-debug                          # 体积优化；完整符号保留在 cmake_build/<preset>/bin
拷贝到 app/src/<debug|release>/jniLibs/arm64-v8a/libaqua.so
拷贝 NDK sysroot 的 libc++_shared.so 到 app/src/main/jniLibs（两 buildType 共用）
```

preset 关键值：`arm64-v8a` / `android-28` / `c++_shared` / Ninja / `arm64-android` triplet /
`AQUA_BUILD_TEST=OFF` / `AQUA_BUILD_APPS=OFF` / `AQUA_BUILD_C_API=ON`。

注意：脚本对 `ANDROID_NDK_HOME` 的检查依赖调用方 shell 的用户级环境变量。若在 CI 或自动化子进程中运行且变量
未传播，先在调用方显式设置。

### 6.2 APK（Gradle）

```powershell
cd aqua_app/aqua_android
.\gradlew.bat assembleDebug      # 调试装机
.\gradlew.bat assembleRelease    # 发布
```

产物：`app/build/outputs/apk/<debug|release>/app-*.apk`。

要点：

```text
jniLibs       AGP 按 buildType 自动合并 sourceSet：debug/release 各取对应 libaqua.so，
              libc++_shared.so 放 main 共享
签名          release 从 aqua_android/keystore.properties 读取（不入 git）；
              文件缺失时回退 debug 签名，产物仍可直接安装
R8            关闭（保护 JNI 动态注册的 FindClass 全名查找）
版本          versionName/versionCode 由根 CMakeLists.txt 的 AQUA_VERSION 派生，单一来源
native 更新   修改 C++ 后必须重跑 build_android.ps1 再打包；Gradle 不会自动重建 native 库
```

### 6.3 安装与验证

```powershell
adb devices
adb -s <device> install -r app\build\outputs\apk\release\app-release.apk
```

真机回归清单（对照 roadmap A5）：连接/断开、自动重连、拔线恢复、屏幕旋转/后台保活（前台服务）、音频焦点、
logcat（tag `aqua`）确认 native 日志。

---

## 7. 运行

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

## 8. Server 网络参数语义

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

## 9. 音频格式与 frames-per-slot

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

## 10. Capture source

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

Windows loopback 在某些设备/驱动场景下，当所有 render client 退出后可能进入 quiescence，使 capture event 暂时完全停止；
切歌等 render 流重建期间还会出现"空事件"（signal 但不产包）与零星小包。

当前 Core 用**欠账驱动**的方式处理，每轮唤醒（事件或 20ms 超时）统一对账：

```text
expected  = 距上轮结算的墙钟欠账（含小数累积）
balance  += expected - 本轮真实交付帧数
balance > 0  → 合成静音补齐（空事件 / 零星小包 / 完全静默同一公式覆盖）
balance < 0  → 记为盈余，抵扣后续欠账
      ↓
原有 callback → Packetizer → Queue → Dispatcher → UDP
```

该设计保持 Packetizer 只有一个 producer，并让下游继续获得连续的音频时间轴——契约上属于采集端职责，client 的 JitterBuffer
只负责网络抖动。细节见 `aqua_core/doc/modules/capture.md`。

### 设备故障与切换

`--device-id` 给出时路由为"指定设备"，省略时"跟随对应方向的系统默认"。设备故障不再终止进程：

```text
设备消失 / 失效 → capture event 上报 DeviceDisconnected
              → control tick（500ms）执行 restart 事务
              → 候选链：目标设备 → 先前的实际设备 → 系统默认
              → 全部失败才停止会话（Fatal）
```

10s 窗口内最多 3 次自动 restart，防止插拔风暴。切换期间 session、格式与 sequence 时间线都不变。

---

## 11. JitterBuffer

默认容量：

```text
30 slots
```

默认阈值：

```text
warning_low   = 20%
normal_low    = 35%
target        = 60%
normal_high   = 80%
warning_high  = 90%
startup_level = 50%   # 启动 pre-roll 锚定水位（独立于稳态阈值序）
```

Warning Fill 是软时间轴校正：重播 READY slot，使 playback timeline 暂时减速；Warning Drop 是跳过完整 slot，使时间轴加速。

默认 step：

```text
1,1,1,1,2,2,2,2,3,3,...
```

并受 `max_step` 限制。Deadline-high 和 reanchor 是更强的恢复路径。

---

## 12. 测试

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
设备切换（CaptureManager / PlaybackManager）
WASAPI backend
```

切换事务用 mock 后端 + mock 设备管理器覆盖，不依赖真实音频设备：候选链（Switched / RolledBack /
FellBackToSystem / Fatal 终态）、重试预算、格式钉死、回调活跃期 restart 无死锁、跟随系统默认变化。

按名字过滤：

```powershell
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug -R CaptureManager
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

## 13. 版本

当前版本单一来源：

```cmake
# 根 CMakeLists.txt
set(AQUA_VERSION "0.2.2")
```

由它派生：

```text
AQUA_CORE_VERSION
AQUA_SERVER_CLI_VERSION
AQUA_CLIENT_CLI_VERSION
AQUA_ANDROID_VERSION
```

`vcpkg.json` 的 `version` 是纯字面量，无法引用 CMake 变量；升级版本时需要手动保持同步。

Android 的 `versionName` / `versionCode` 由 Gradle 直接读取根 `CMakeLists.txt` 的版本字面量并按同一算法派生
（major×1_000_000 + minor×1_000 + patch），无独立版本源。

---

## 14. 常见构建问题

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

### Android：`ANDROID_NDK_HOME is not set`

脚本依赖调用方 shell 的用户级环境变量。确认：

```powershell
echo $env:ANDROID_NDK_HOME
```

在 CI / 自动化子进程中变量可能不传播，需在调用方显式设置后再运行 `build_android.ps1`。

### Android：装机后 JNI 签名不匹配闪退

修改了 C++/JNI 接口后未重跑 `build_android.ps1`，APK 内仍是旧 `libaqua.so`。重跑脚本并重新打包。

### Android：debug 与 release 签名冲突

`INSTALL_FAILED_UPDATE_INCOMPATIBLE`：设备上的包与 APK 签名不一致（如先装 debug 后装正式签名 release）。
卸载旧包后安装；切换签名会使应用数据（保存的服务器配置等）清空。

---

## 15. 构建与文档关系

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

Android 打包与同步脚本的权威实现：

```text
aqua_app/aqua_android/build_android.ps1
aqua_app/aqua_android/app/build.gradle.kts
```

顶层 README 用于项目介绍和快速上手，不应重新定义一套独立的配置语义。
