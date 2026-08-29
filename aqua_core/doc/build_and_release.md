# 构建与发布

## 1. 标准

当前工程使用：

- C++23
- CMake 4.2+
- vcpkg manifest
- Asio
- protobuf / gRPC
- spdlog
- GoogleTest/CTest

## 2. Core targets

```text
aqua_proto
aqua_core_base
aqua_server_core
aqua_client_core
```

应用层由 `aqua_app/CMakeLists.txt` 构建。

## 3. Windows

```text
windows-x64-debug
windows-x64-release
```

Debug 定义 `AQUA_DEBUG`。Release 不定义。

## 4. Android 当前预留

工程已经有：

```text
android-arm64-debug
android-arm64-release
```

使用 NDK toolchain、`arm64-v8a`、`ANDROID_PLATFORM=android-28`、`c++_shared`、Ninja。当前这些 preset 只说明交叉编译骨架存在，不代表 AAudio backend / JNI / Android app 已经完成。

## 5. 版本

顶层 `CMakeLists.txt` 中 `AQUA_VERSION` 是单一版本源；Core、CLI 版本头从它派生。`vcpkg.json` 的 version 无法引用 CMake 变量，所以改版本时必须同步。

## 6. 发布前检查

1. Debug/Release 构建无 warning regression；
2. CTest 全绿；
3. CLI `--help` 与文档一致；
4. server/client 的实际音频格式和 F 有运行日志；
5. UDP advertised endpoint 在真实部署地址上验证；
6. RT debug log 不应作为生产性能基线；
7. Android 包必须同时验证 native ABI、`libc++_shared.so` 和 Java/Kotlin 层 release packaging。
