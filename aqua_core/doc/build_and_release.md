# 构建与发布

## 1. 标准与依赖

```text
C++23，CMake 4.2+，vcpkg manifest（spdlog / cxxopts / grpc / asio，feature tests → gtest）
protobuf 由 grpc 传递引入，不在 manifest 中单独声明
```

**gRPC 链接 unsecure 变体**：`aqua_proto` 链接的是 `gRPC::grpc++_unsecure`，不是 `gRPC::grpc++`。
控制面两端都用 `Insecure{,Server}Credentials`（线路上本来就没有 TLS，见 `design_decisions.md` D8），
所以换成不含安全层的变体**对运行时行为零影响** —— 吞吐、延迟、连接建立速度都不变，
唯一收益是不把 TLS / BoringSSL 代码编进产物（对 Android 的 `libaqua.so` 体积有意义）。
将来若要上 TLS 或鉴权：把这一项改回 `gRPC::grpc++` 即可，**业务代码不用改**
（`Insecure*` / `CreateChannel` / `ServerBuilder` / `Status` 在 unsecure 变体里同样导出）。

## 2. 目标

```text
aqua_proto        proto 生成代码 + audio_format_converter（唯一依赖 protobuf 的基础目标）
aqua_core_base    logger / diagnostics / session / address / UdpTransport / NetworkFrame / 设备管理器
aqua_server_core  GrpcServer / UdpServer / AudioCapture / CaptureManager / Packetizer / ServerRuntime
aqua_client_core  GrpcClient / UdpClient / JitterBuffer / AudioPlayback / PlaybackManager / ClientRuntime
aqua_capi         C API + JNI，产物为共享库 aqua / libaqua.so（默认 OFF）
aqua_server_cli   控制台 server（aqua_app/cli）
aqua_client_cli   控制台 client（aqua_app/cli）
```

采集与回放分属两个 core target，平台后端依赖不会传播到另一端。

### CMake 开关

| 选项                               | 默认 | 说明                                           |
|------------------------------------|------|------------------------------------------------|
| `AQUA_BUILD_SERVER_CORE`           | ON   | server 侧核心                                  |
| `AQUA_BUILD_CLIENT_CORE`           | ON   | client 侧核心                                  |
| `AQUA_BUILD_APPS`                  | ON   | 两个 CLI                                       |
| `AQUA_BUILD_TEST`                  | ON   | 测试目标                                       |
| `AQUA_BUILD_C_API`                 | OFF  | C API / JNI（Android 与 windows preset 打开）  |
| `AQUA_DEBUG`                       | OFF  | Debug 附加断言                                 |
| `AQUA_SERVER_RT_DEBUG_LOG`         | OFF  | 开发期开关（server 音频实时链日志），**会破坏 RT 契约** |
| `AQUA_CLIENT_RT_DEBUG_LOG`         | OFF  | 开发期开关（client 音频实时链日志），**会破坏 RT 契约** |
| `AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG` | OFF | 开发期开关（JB target 决策层日志），不破坏 RT 契约 |

平台后端按条件编入：WASAPI 仅 Windows，AAudio 仅 Android。Linux / macOS 可以配置并编译通过，但没有任何音频后端。

## 3. Preset

| Preset                | 生成器             | 说明                                                                 |
|-----------------------|--------------------|----------------------------------------------------------------------|
| `windows-x64-debug`   | VS 18 2026（x64）  | Debug 定义 `AQUA_DEBUG`                                              |
| `windows-x64-release` | VS 18 2026（x64）  |                                                                      |
| `linux-x64-*`         | Ninja Multi-Config | 无音频后端                                                           |
| `macos-arm64-*`       | Ninja Multi-Config | 无音频后端                                                           |
| `android-arm64-*`     | Ninja              | `arm64-v8a`、`android-28`、`c++_shared`；关闭 apps/tests，打开 C API |

build preset 有 8 个； **test preset 只有 6 个**（不含 Android）。两个 Android preset 没有 `condition` 字段，因此在任何主机上
都可见，但实际依赖 Windows 路径下的 NDK 工具链。

### 测试覆盖不对称（已知事实，别把它当 bug 或当已覆盖）

`AQUA_BUILD_TEST` 默认 `ON`（只有 `android-arm64-*` 关掉），但 `aqua_capi_test` 还需要
`AQUA_BUILD_C_API=ON`。目前打开它的只有 `android-arm64-*`（**不跑测试**）与 `windows-x64-debug` /
`windows-x64-release`。结论：

- **C API / JNI 的位置契约（`AQUA_DIAGNOSTICS_FIELD_COUNT`、枚举镜像）实际由 windows 的两个配置覆盖**；
  Android 侧虽然编了 `aqua_capi` / `aqua_jni.cpp`（有 `static_assert` 兜住写入条数），但从不执行测试。
- `windows-x64-release` 曾漏开该开关，后果是 Release 用例数比 Debug 少 7 条（436 vs 443）且 C ABI 在
  Release 完全没有测试；现有 parity 是 443/443。改 preset 后必须重新 `cmake --preset <name>`——`cmake
  --build` 只会重跑 configure 并**沿用缓存**，preset 里的改动不会自动生效。

## 4. Android 构建

`aqua_app/aqua_android/build_android.ps1` 串联以下步骤：

```text
cmake --preset android-arm64-{debug,release}
cmake --build <preset dir> --target aqua_capi
llvm-strip --strip-debug（NDK 路径，脚本内硬编码 windows-x86_64 宿主）
拷贝 libaqua.so -> app/src/<debug|release>/jniLibs/arm64-v8a/
拷贝 NDK sysroot 的 libc++_shared.so -> app/src/main/jniLibs/arm64-v8a/
Gradle 打包 APK
```

**兼容层必须保留**：`../include/aqua/compat/move_only_function.h` 是回调类型（capture / playback / udp / grpc） 的统一声明入口——NDK
r30（clang 21）的 libc++ 至今没有 `std::move_only_function`
（`__cpp_lib_move_only_function` 未定义），故 libc++ 侧回退 `std::function`，MSVC 侧直接用原生类型。 **不要**用"LLVM
版本够新"推断 libc++ 特性：删之前必须在 Android preset 上实测（用该类型编一个 TU 即可）。

`-SkipDebug` / `-SkipRelease` 可只跑一半。完整流程见仓库根 `BUILD.md`。

## 5. 版本

顶层 `CMakeLists.txt` 的 `AQUA_VERSION`（当前 `0.3.0`）是单一版本源，派生出 `AQUA_CORE_VERSION`、
`AQUA_SERVER_CLI_VERSION`、`AQUA_CLIENT_CLI_VERSION`；**Android 侧不消费 CMake 变量**，由
`app/build.gradle.kts` 直接解析该字面量并自算 versionName/versionCode。改版本时必须同步根目录
`vcpkg.json` 的 `version` 字段（它无法引用 CMake 变量）。

## 6. 发布前检查

1. Debug / Release 构建无新增 warning；
2. `ctest --preset windows-x64-debug` 与 release 全绿；
3. CLI `--help` 与 `configuration_reference.md` 一致；
4. 启动日志能确认实际音频格式与 F；
5. UDP advertised endpoint 在真实部署地址（含 NAT / 端口映射）上验证；
6. RT debug log 不得作为生产性能基线；
7. Android 包同时验证 native ABI、`libc++_shared.so` 与 release 打包；
8. 发布产物目录 `release/` 不入版本库。
