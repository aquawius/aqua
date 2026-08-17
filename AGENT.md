# Aqua — Agent 工作指南

Aqua 是跨平台低延迟网络音频共享系统：一台设备采集 PCM 音频，经 UDP 实时传输到另一台回放。 技术栈 C++23 + CMake + vcpkg；UI 与
core 完全解耦（C API / JNI）。

## 快速开始

构建命令详见 [build.md](build.md)，核心流程：

```powershell
# Windows 桌面
cmake --preset windows-x64-debug
cmake --build cmake_build/windows-x64-debug --config Debug
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug --output-on-failure

# Android（先编 native 库，再打 APK）
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1
cd Android; .\gradlew.bat assembleDebug    # 或 assembleRelease
```

## 目录结构（当前）

```text
aqua/
├── CMakeLists.txt             # 顶层构建 + 版本号单一来源
├── CMakePresets.json          # win / linux / macos / android presets
├── vcpkg.json                 # 依赖清单（manifest 模式）
├── include/aqua.h             # C API 公共头（UI ↔ Core 的 C ABI）
├── proto/aqua_service.proto   # gRPC 控制面协议
├── src/
│   ├── core/                  # 核心库
│   │   ├── public/            #   audio_format.h / config.h / version.h.in
│   │   ├── audio/             #   backend（wasapi / aaudio）+ ringbuffer
│   │   ├── jitter_buffer/
│   │   ├── net/               #   transport（UDP）+ packet（二进制编解码）
│   │   ├── grpc/              #   grpc_server / grpc_client / format_converter
│   │   ├── server/ client/    #   运行时编排（ServerRuntime / ClientRuntime）
│   │   ├── session/           #   SessionManager
│   │   ├── diagnostics/       #   DiagnosticsManager
│   │   ├── logger/            #   spdlog 封装
│   │   └── capi/              #   C API 实现
│   ├── app/cli/               # CLI 前端（server / client + 解析器 + cli_version.h.in）
│   └── android/jni/           # JNI 薄桥（Kotlin ↔ aqua.h，动态注册）
├── Android/                   # Android App（Kotlin/Compose + 前台媒体服务）
├── tests/                     # 单测/集成（镜像 src 布局）
└── doc/                       # 详细设计文档
```

## CMake 目标

| Target        | 说明                                                        |
|---------------|-------------------------------------------------------------|
| `aqua_proto`  | proto 生成的 `*.pb.cc` / `*.grpc.pb.cc`（STATIC）           |
| `aqua_core`   | 核心库（STATIC）                                            |
| `aqua_capi`   | C ABI（桌面 STATIC；Android SHARED = `libaqua.so`，含 JNI） |
| `aqua_server` | Server CLI（链接 `aqua_core` + cxxopts，Android 不构建）    |
| `aqua_client` | Client CLI（同上）                                          |
| `aqua_tests`  | GoogleTest                                                  |

## 架构边界（写代码前必读）

- **控制面 / 数据面分离**：gRPC 只做 Connect / Disconnect；音频走 UDP。gRPC 不承载音频、不参与保活。
- **Server 不做音频转换**：不重采样 / 转码 / 混音；Client 自负格式转换。
- **音频格式同步**：`src/core/public/audio_format.h` 的原生 `AudioEncoding` 数值必须与
  `proto/aqua_service.proto` 的 `AudioFormat.Encoding` 一一对应（`aqua_capi.cpp` 有 static_assert 校验）。
- **热路径无锁无分配**：audio callback / UDP 收发 / JitterBuffer push-pop 禁止动态分配与阻塞。
- **平台代码只放 `audio/backend`**：wasapi / aaudio 通过 `audio_backend_factory.h` 抽象暴露，不得泄漏平台头。
- **SessionManager 只存状态**：session_id / endpoint / created_at / last_seen / state，不依赖 net / grpc / audio。
- **版本号单一来源**：根 `CMakeLists.txt` 顶部 `AQUA_*_VERSION`，经 `configure_file` 生成
  `core/public/version.h` 与 `app/cli/cli_version.h`；Android `versionName`/`versionCode` 由 Gradle 直读。
- **C API 是 UI 唯一入口**：`include/aqua.h`，只暴露不透明句柄，跨边界不抛异常、不传 C++ 类型。

## 详细设计文档

| 文档                                                     | 内容                                                                             |
|----------------------------------------------------------|----------------------------------------------------------------------------------|
| [doc/architecture.md](doc/architecture.md)               | 目标 / 技术栈 / 分层 / 数据流 / 线程模型 / 低延迟原则 / 依赖图 / 并发 / 错误处理 |
| [doc/protocol.md](doc/protocol.md)                       | AudioFormat / gRPC / NAT / UDP 包 / proto 定义                                   |
| [doc/modules.md](doc/modules.md)                         | SessionManager / RingBuffer / JitterBuffer / 模块接口 / C API / 配置 / 日志      |
| [doc/roadmap.md](doc/roadmap.md)                         | 里程碑 / 明确不做 / 实现状态                                                     |
| [doc/diagnostics_manager.md](doc/diagnostics_manager.md) | 诊断数据采集与输出                                                               |
| [build.md](build.md)                                     | 构建指南                                                                         |
