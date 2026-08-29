# Build & Release

## 1. Requirements

当前根 CMake：

```text
cmake_minimum_required(VERSION 4.2)
C++23
```

Windows 主验证链：

```text
Visual Studio 18 2026 / MSVC
vcpkg
Asio
gRPC + protobuf
spdlog
cxxopts
GoogleTest
```

## 2. Presets

主要 presets：

```text
windows-x64-debug
windows-x64-release
linux-x64-debug
linux-x64-release
macos-arm64-debug
macos-arm64-release
android-arm64-debug
android-arm64-release
```

## 3. Server/Client core split

```text
aqua_core_base
aqua_server_core
aqua_client_core
```

Server core 只包含 capture 方向代码；Client core 只包含 playback/JitterBuffer；device manager 放在 base。

## 4. Debug switches

普通 debug：

```text
AQUA_DEBUG=ON
```

JitterBuffer realtime debug logging：

```text
AQUA_JITTER_BUFFER_RT_DEBUG_LOG=ON
```

两者语义不同；后者不能成为 release default。

## 5. Release checklist

- Release build；
- `AQUA_JITTER_BUFFER_RT_DEBUG_LOG=OFF`；
- 全量 unit/integration tests；
- `--list-devices` 可用；
- 默认音频格式来自 backend；
- wildcard advertise fallback regression test；
- server/client lifecycle regression；
- 文档与 CLI help 无旧配置/旧术语。
