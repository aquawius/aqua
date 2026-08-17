# Aqua 构建指南

本文档说明 Aqua 的完整构建链路与常用命令。构建分 **两段**：

1. **Native（C++）**：CMake + vcpkg + NDK 交叉编译，产出 `libaqua.so`。
2. **Android APK**：Gradle 打包预编译的 `.so` + 编译 Kotlin/Compose。

> 桌面端（Windows/Linux/macOS）的 CLI 走同一套 CMake，见下方「桌面 CLI」。

---

## 1. 前置环境

| 工具        | 说明                         | 环境变量                                |
|-------------|------------------------------|-----------------------------------------|
| CMake ≥ 4.2 | 配置与构建 native            | —                                       |
| vcpkg       | manifest 模式，依赖锁定      | `VCPKG_ROOT`                            |
| Android NDK | 交叉编译工具链               | `ANDROID_NDK_HOME`                      |
| Android SDK | Gradle 打包（compileSdk 37） | `sdk.dir`（`Android/local.properties`） |
| JDK         | Gradle 运行                  | —                                       |

本机示例：

```powershell
$env:VCPKG_ROOT = "C:\CodingDeps\vcpkg"
$env:ANDROID_NDK_HOME = "C:\CodingDeps\Android\Sdk\ndk\30.0.15729638"
```

依赖清单在仓库根 `vcpkg.json`（grpc / protobuf / asio / spdlog / cxxopts / gtest）。

---

## 2. 构建链路总览

```text
┌─────────────────────────────────────────────────────────────────────┐
│ ① Native（CMake + vcpkg + NDK）                                     │
│   cmake --preset android-arm64-{debug|release}                      │
│   cmake --build cmake_build/android-arm64-* --target aqua_capi      │
│        └─> libaqua.so（aqua_core + aqua_capi + JNI）                 │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ 同步到 per-buildType jniLibs
                               ▼
   Android/app/src/debug/jniLibs/arm64-v8a/libaqua.so    （Debug，含符号）
   Android/app/src/release/jniLibs/arm64-v8a/libaqua.so   （Release，-O2 无 DWARF）
   Android/app/src/main/jniLibs/arm64-v8a/libc++_shared.so （从 NDK 拷贝，共享）

┌─────────────────────────────────────────────────────────────┐
│ ② APK（Gradle）                                             │
│   gradlew assembleDebug / assembleRelease                   │
│        └─> AGP 按 buildType 取对应 jniLibs + 编译 Kotlin      │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Android 构建

### 3.1 构建 native 库（一键脚本，推荐）

在 **仓库根目录**执行：

```powershell
# 构建 debug + release 两个 native 库并同步到 jniLibs
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1

# 只更新 release
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1 -SkipDebug

# 只更新 debug
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1 -SkipRelease
```

脚本内部等价于：

```powershell
# debug
cmake --preset android-arm64-debug
cmake --build cmake_build/android-arm64-debug --target aqua_capi
Copy-Item cmake_build/android-arm64-debug/libaqua.so Android/app/src/debug/jniLibs/arm64-v8a/ -Force

# release
cmake --preset android-arm64-release
cmake --build cmake_build/android-arm64-release --target aqua_capi
Copy-Item cmake_build/android-arm64-release/libaqua.so Android/app/src/release/jniLibs/arm64-v8a/ -Force
# 去符号（否则 release .so 因静态依赖的 DWARF 高达 ~300 MB）
& "$env:ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" --strip-debug Android/app/src/release/jniLibs/arm64-v8a/libaqua.so

# libc++_shared.so（ANDROID_STL=c++_shared 必需，从 NDK 拷贝）
Copy-Item "$env:ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" Android/app/src/main/jniLibs/arm64-v8a/ -Force
```

### 3.2 打包 APK

在 `Android/` 目录：

```powershell
.\gradlew.bat assembleDebug      # debug APK
.\gradlew.bat assembleRelease    # release APK（发布用，体积小）
```

产物：

- `Android/app/build/outputs/apk/debug/app-debug.apk`
- `Android/app/build/outputs/apk/release/app-release.apk`

> release 目前用 **debug 签名**（`build.gradle.kts` 里 `signingConfig = debug`），
> 直接可安装；正式发布前需换成正式签名。

---

## 4. 桌面 CLI 构建

桌面端（Windows / Linux / macOS）也是同一套 CMake，只是 preset 不同。

```powershell
# Windows x64 Debug
cmake --preset windows-x64-debug
cmake --build cmake_build/windows-x64-debug --config Debug
ctest --test-dir cmake_build/windows-x64-debug --build-config Debug --output-on-failure

# Windows x64 Release
cmake --preset windows-x64-release
cmake --build cmake_build/windows-x64-release --config Release
```

产物：`aqua_server.exe` / `aqua_client.exe`（CLI），以及 `aqua_core.lib` / `aqua_capi.lib`。

---

## 5. CMake Presets

| Preset                                          | 平台              | 说明                                          |
|-------------------------------------------------|-------------------|-----------------------------------------------|
| `windows-x64-debug` / `windows-x64-release`     | Windows x64       | MSVC + vcpkg `x64-windows`                    |
| `android-arm64-debug` / `android-arm64-release` | Android arm64-v8a | Ninja + vcpkg `arm64-android` + NDK chainload |
| `linux-x64-debug` / `linux-x64-release`         | Linux x64         | Ninja Multi-Config + vcpkg `x64-linux`        |
| `macos-arm64-debug` / `macos-arm64-release`     | macOS ARM64       | Ninja Multi-Config + vcpkg `arm64-osx`        |

所有 preset 继承 `base`：`CXX_STANDARD=23`、`BUILD_TESTS=ON`（Android 关掉）、
`VCPKG_INSTALLED_DIR=${sourceDir}/vcpkg_installed`、导出 `compile_commands.json`。

---

## 6. 版本号

版本号 **单一来源**是仓库根 `CMakeLists.txt` 顶部：

```cmake
set(AQUA_CORE_VERSION "0.1.0")
set(AQUA_SERVER_CLI_VERSION "0.1.0")
set(AQUA_CLIENT_CLI_VERSION "0.1.0")
set(AQUA_ANDROID_VERSION "0.1.0")
set(AQUA_ANDROID_VERSION_CODE 1)
```

- `aqua_server --version` / `aqua_client --version`：读各自 CLI 版本宏。
- Android `versionName` / `versionCode`：`Android/app/build.gradle.kts` 直接读上面的 CMake 变量。

---

## 7. 体积说明

| 库                   | strip 前   | strip 后   | 说明                                       |
|----------------------|-----------|-----------|--------------------------------------------|
| debug `libaqua.so`   | ~438 MB   | ~20–30 MB | `-g` 全 DWARF 调试符号，**仅进 debug APK** |
| release `libaqua.so` | ~300 MB   | ~10–20 MB | `-O2` 但静态依赖仍带 DWARF；脚本已自动 strip |
| `libc++_shared.so`   | ~9 MB     | ~9 MB     | C++ 标准库共享，debug/release 都要         |

> 关键点：`libaqua.so` 里**静态链入了 gRPC / protobuf / abseil / openssl 等**，这些
> vcpkg 依赖自带 DWARF 调试信息，所以即便 `CMAKE_BUILD_TYPE=Release`，不 strip 时
> release `.so` 仍高达 ~300 MB。`build_android.ps1` 在同步 release 库后会调用 NDK 的
> `llvm-strip --strip-debug` 去掉调试信息，APK 才真正变小。

所以：

- **debug APK 会很大**（~450 MB），只用于开发调试，属正常现象。
- **release APK 很小**（~15–30 MB），因为 strip 掉了 release `libaqua.so` 的调试信息。

---

## 8. 常见问题

### 8.1 vcpkg/cmake 报 `permission denied`（buildtrees/...）

通常是 **文件被别的进程占用**，最常见两种：

1. **IDE（Android Studio / CLion）正在后台跑 cmake**，和命令行抢同一个 cmake/vcpkg 构建目录。
2. `aqua_server.exe` / `aqua_client.exe` 还在运行，占用了构建产物。

**解决**：关闭 IDE、退出正在运行的 aqua 可执行文件，再重新构建。

```powershell
Get-Process | Where-Object { $_.ProcessName -like 'aqua*' }   # 检查 aqua 进程
Stop-Process -Name aqua_server,aqua_client -Force              # 如有则结束
```

### 8.2 运行 APK 报 `UnsatisfiedLinkError: dlopen libaqua.so`

说明对应 buildType 的 `jniLibs` 里缺 `libaqua.so`。先跑一次：

```powershell
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1
```

确认 `Android/app/src/{debug,release}/jniLibs/arm64-v8a/libaqua.so` 存在后再打包。

### 8.3 首次构建触发 vcpkg 安装（耗时长）

首次 `cmake --preset android-arm64-*` 会经 vcpkg manifest 安装 `arm64-android` 依赖 （grpc / protobuf 等），需要较长时间；之后增量构建很快。

---

## 9. 一键流程（日常开发）

```powershell
# 在仓库根目录
powershell -ExecutionPolicy Bypass -File Android/build_android.ps1   # 构建并同步 native

# 打 APK（在 Android/）
cd Android
.\gradlew.bat assembleDebug    # 或 assembleRelease
```
