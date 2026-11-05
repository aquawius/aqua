# 构建与发布

## 1. 标准与依赖

```text
C++23，CMake 4.2+，vcpkg manifest（spdlog / cxxopts / grpc / asio；feature `tests` → gtest，feature `gui` → imgui[sdl3-binding,sdl3-renderer-binding] + sdl3，后者仅桌面 GUI 使用）
protobuf 由 grpc 传递引入，不在 manifest 中单独声明
```

### vcpkg baseline 策略

`vcpkg.json` 的 `builtin-baseline` 是**依赖图基线**，与 `vcpkg.json` 里那个"本项目自己的 `version` 字段"
毫无关系，别混。当前钉 `e456309491fd875487bf02b8a0dca76c1b1b00cc`（2026-10-10，master 尖端；
随 Visual Studio 升级一起换的 —— 理由见下一节）。

与上一版基线 `dc31f86a441067d6c2e9e4ad52988eb41ef4f442`（2026-08-11）相比，**依赖闭包内只有一项变化**：
OpenSSL **3.6.3 → 3.6.5**；其余 grpc **1.81.1**、protobuf **6.33.4**、abseil **20260107.1**、
spdlog **1.17.0**、asio **1.32.0**、cxxopts **3.3.1**、zlib **1.3.2**、c-ares **1.34.8**、
re2 **2025-11-05**、fmt **12.2.0**、gtest **1.18.0** 全部不变（做法：用 `git ls-tree <base>:ports`
取每个端口的目录树，再到该基线的 `versions/<首字母>-/<port>.json` 里按 `git-tree` 反查版本号；
两个基线各跑一遍逐项比对。⚠️ 那些 `versions` 条目里的字段名不统一，`version` / `version-semver` /
`version-date` / `version-string` 都要认，只看 `version` 会得到一片"未收录"的假象）。
⇒ 净收益很小（一次 patch 级 OpenSSL 升级），选在这个时间点纯粹是因为**重建本来就要发生**。

⚠️ 上表的数字来自**安装结果**（`<triplet>/share/<port>/vcpkg.spdx.json`），换代基线后需重建回填。

**只在三种情况下更新基线**：① 上游安全修复影响到本项目的用法；② 需要新版本独有的功能或行为修复；
③ 要同步到某个 tagged release（vcpkg 建议以 tag 作基线，而不是 master 上的提交）。

**代价与规矩**：

- 基线一变，**四个 build 目录各自的 `vcpkg_installed` 都要重装/重编**（实测 grpc 的 rel+dbg 两配置
  合计 ≈15 分钟，openssl 走 perl+make 另计）⇒ 数小时 + 十几 GB 磁盘；
- 它**必然触发 configure** ⇒ 要在能跑 `cmake --preset` 的环境里做；
- **单独一个提交**（`chore(deps): bump the vcpkg baseline to …`），并在 4 个 preset 上
  全量构建 + 全量 ctest 之后才能合；
- 前提：**目标提交必须已经在本地 vcpkg 克隆里**（vcpkg 从本地 `versions/` 读版本索引）⇒ 先 `git fetch`。
  核对某基线的日期：`git -C "$VCPKG_ROOT" log -1 --format=%H%n%ad --date=short <baseline>`。
- **只想升单个端口**时，用 `vcpkg.json` 的 `"overrides"` 钉那一个版本（该版本的 git-tree 需在本地 registry 里），
  影响面远小于整体换基线。

**gRPC 链接 unsecure 变体**：`aqua_proto` 链接的是 `gRPC::grpc++_unsecure`，不是 `gRPC::grpc++`。
控制面两端都用 `Insecure{,Server}Credentials`（线路上本来就没有 TLS，见 `design_decisions.md` D8），
所以换成不含安全层的变体**对运行时行为零影响** —— 吞吐、延迟、连接建立速度都不变，
唯一收益是不把 TLS / BoringSSL 代码编进产物（对 Android 的 `libaqua.so` 体积有意义）。
将来若要上 TLS 或鉴权：把这一项改回 `gRPC::grpc++` 即可，**业务代码不用改**
（`Insecure*` / `CreateChannel` / `ServerBuilder` / `Status` 在 unsecure 变体里同样导出）。

### 升级 vcpkg（或换 Visual Studio）的完整流程

**先分清两件事，它们彼此独立**：① **vcpkg 本体** —— `%VCPKG_ROOT%` 的 git 检出 + `vcpkg.exe`；
② `vcpkg.json` 的 `builtin-baseline` —— 依赖版本图。**改 baseline 不必换本体，换本体也不改 baseline**
（本体版本与依赖版本无关），但两者都要重新 configure。

**换 VS 会让依赖全量重编 —— 这不是推测**：`scripts/detect_compiler/CMakeLists.txt` 算出的 `COMPILER_HASH`
是**编译器二进制的 SHA1**（`file(SHA1 "${CMAKE_CXX_COMPILER}")`，另有 size+mtime 短路缓存），它是 ABI 的输入。
所以 VS 一升、`cl.exe` 一变 ⇒ **所有端口 ABI 全变** ⇒ `%LOCALAPPDATA%/vcpkg/archives` 里那 14 GB
二进制缓存**一条都命中不了**。只有源码 tarball（同目录下 `downloads`，1.8 GB）还留着 ⇒ **只重编、不重下**。

⇒ **结论：换 VS 与升 vcpkg 应当合并成一次全量重建**，不要分两轮各编一遍。流程：

```text
0) 停掉占用 exe 的进程（否则链接报 LNK1104）
     Get-Process aqua_server_cli, aqua_client_cli -EA SilentlyContinue | Stop-Process -Force

1) 升级 vcpkg 本体
     cd $env:VCPKG_ROOT                    # 本机 = C:/CodingDeps/vcpkg
     git fetch --tags origin
     git log -1 --format="%H %ad %s" --date=short origin/master   # 先看清目标再动
     git checkout master
     git pull --ff-only
     ./bootstrap-vcpkg.bat -disableMetrics   # 必须：重建 vcpkg.exe，老 exe 读不懂新端口版本方案
     vcpkg version
     git rev-parse HEAD                      # 记下要钉的基线提交

2) 把该提交写进 aqua/vcpkg.json 的 builtin-baseline（只升本体则跳过这步）

3) 旧树作废：VS 换了 + ABI 全变，留着只会添乱（cmake_build 已被 gitignore）
     cd D:/coding/aqua
     Remove-Item -Recurse -Force cmake_build

4) 四个 preset 从零 configure，再构建
     foreach ($p in 'windows-x64-debug','windows-x64-release','android-arm64-debug','android-arm64-release') {
         cmake --preset $p; if ($LASTEXITCODE) { break }
     }
     cmake --build --preset windows-x64-debug
     cmake --build --preset windows-x64-release
     cmake --build --preset android-arm64-debug
     cmake --build --preset android-arm64-release

5) 全量 ctest（两个 Windows 配置都要，-C 已写在 testPreset 里）
     ctest --preset windows-x64-debug
     ctest --preset windows-x64-release
```

**注意点**

- **最新 tag 是 `2026.07.29`，比现钉的基线（2026-08-11）还旧** ⇒ 不存在"更新的 tag"可跟；
  要么钉 `origin/master` 尖端提交（**2026-10-10 那次即钉了 `e456309491`**），要么**只升本体、baseline 不动**。
  分开做的好处是**可二分**：先"换 VS + 升本体 + 同 baseline"重建到绿，再单独一个提交升 baseline。
- 换 VS 后先确认 generator 字符串还对：preset 里写死 `"Visual Studio 18 2026"`，
  跨大版本（如 18 → 19）必须同步改（`cmake --help` 可列出本机可用 generator）。
- 磁盘：四个 preset 的 `cmake_build` 合计 ≈58 GB；`%VCPKG_ROOT%` 自己的
  `buildtrees` / `packages` 另有 ≈62 GB，重建前按需清理。
- 只升**单个端口**别动 baseline，用 `vcpkg.json` 的 `"overrides"`。

## 2. 目标

```text
aqua_proto        proto 生成代码 + audio_format_converter（唯一依赖 protobuf 的基础目标）
aqua_core_base    logger / diagnostics / session / address / UdpTransport / NetworkFrame / 设备管理器
aqua_server_core  GrpcServer / UdpServer / AudioCapture / CaptureManager / Packetizer / ServerRuntime
aqua_client_core  GrpcClient / UdpClient / JitterBuffer / AudioPlayback / PlaybackManager / ClientRuntime
aqua_capi         C API + JNI，产物为共享库 aqua / libaqua.so（默认 OFF）
aqua_server_cli   控制台 server（aqua_app/cli）
aqua_client_cli   控制台 client（aqua_app/cli）
aqua_client_gui   桌面图形客户端（aqua_app/gui/client，Dear ImGui + SDL3，直链 aqua_client_core）
aqua_server_gui   桌面图形服务端（aqua_app/gui/server，复用 CLI 参数解析，直链 aqua_server_core）
```

采集与回放分属两个 core target，平台后端依赖不会传播到另一端。

### CMake 开关

| 选项                               | 默认 | 说明                                           |
|------------------------------------|------|------------------------------------------------|
| `AQUA_BUILD_SERVER_CORE`           | ON   | server 侧核心                                  |
| `AQUA_BUILD_CLIENT_CORE`           | ON   | client 侧核心                                  |
| `AQUA_BUILD_APPS`                  | ON   | 两个 CLI                                       |
| `AQUA_BUILD_GUI`                   | ON   | 桌面 GUI（`aqua_client_gui`）                   |
| `AQUA_BUILD_TEST`                  | ON   | 测试目标                                       |
| `AQUA_BUILD_C_API`                 | OFF  | C API / JNI（Android 与 windows preset 打开）  |
| `AQUA_DEBUG`                       | OFF  | Debug 附加断言                                 |
| `AQUA_SERVER_RT_DEBUG_LOG`         | OFF  | 开发期开关（server 音频实时链日志），**会破坏 RT 契约** |
| `AQUA_CLIENT_RT_DEBUG_LOG`         | OFF  | 开发期开关（client 音频实时链日志），**会破坏 RT 契约** |
| `AQUA_CLIENT_JB_TARGET_CONTROL_DEBUG_LOG` | OFF | 开发期开关（JB target 决策层日志），不破坏 RT 契约 |

平台后端按条件编入：WASAPI 仅 Windows，AAudio 仅 Android。Linux / macOS 可以配置并编译通过，但没有任何音频后端
（GUI 能开、设备枚举为空、无法连接）。

**GUI 依赖独立性**：imgui / sdl3 只声明在 `vcpkg.json` 的 manifest feature `gui` 里，桌面 preset
（windows / linux / macos）的 `VCPKG_MANIFEST_FEATURES` 为 `tests;gui`，configure 时自动安装。
不构建 GUI：

```powershell
cmake --preset windows-x64-debug -DAQUA_BUILD_GUI=OFF -DVCPKG_MANIFEST_FEATURES=tests
```

两个开关都传才生效：只传 `-DAQUA_BUILD_GUI=OFF` 而忘记关 feature 时，vcpkg 仍会安装依赖（仅不构建 target）。
Linux 上 SDL3 另需系统开发包（X11 / Wayland）。

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
  Release 完全没有测试；现有 parity 是 **565/565**（2026-10-10 实测，Debug 与 Release 相同）。改 preset 后必须重新 `cmake --preset <name>`——`cmake
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

> **`libc++_shared.so` 也由脚本 strip**（2026-10-10 起）：从 NDK sysroot 拷贝后立即调用同一个
> `Strip-Native`，实测 `9,485,032 → 1,867,040` 字节，debug / release 两个 APK 各减约 **7.6 MB**。
>
> 为什么不交给 AGP：`app/build.gradle.kts` 未声明 `ndkVersion` 时，AGP 会去它**内置的默认版本**
> （9.3.3 = `28.2.13676358`）找 `llvm-strip`，本机装的是别的版本 ⇒ 找不到 ⇒`stripDebugDebugSymbols`
> **静默退化为"原样打包"**，只在日志里留一条 warning。而声明 `ndkVersion` 意味着把**机器本地属性**
> 钉死（会绑住其他协作者的 NDK 版本），所以改为脚本侧解决。
>
> ⚠️ 加了脚本 strip 之后，那条 `Unable to strip the following libraries …` warning **仍会出现**
> （AGP 依然找不到工具），但**产物内容是对的** —— 判据看 `.debug_` 段表，不要看这条 warning。

**兼容层必须保留**：`../include/aqua/compat/move_only_function.h` 是回调类型（capture / playback / udp / grpc） 的统一声明入口——NDK
r30（clang 21）的 libc++ 至今没有 `std::move_only_function`
（`__cpp_lib_move_only_function` 未定义），故 libc++ 侧回退 `std::function`，MSVC 侧直接用原生类型。 **不要**用"LLVM
版本够新"推断 libc++ 特性：删之前必须在 Android preset 上实测（用该类型编一个 TU 即可）。

`-SkipDebug` / `-SkipRelease` 可只跑一半。完整流程见仓库根 `BUILD.md`。

## 5. 版本

顶层 `CMakeLists.txt` 的 `AQUA_VERSION`（当前 `0.3.1`）是单一版本源，派生出 `AQUA_CORE_VERSION`、
`AQUA_SERVER_CLI_VERSION`、`AQUA_CLIENT_CLI_VERSION`；**Android 侧不消费 CMake 变量**，由
`app/build.gradle.kts` 直接解析该字面量并自算 versionName/versionCode。

改版本时要同步的**全部落点**（这些地方只能复制字面量，引用不到 CMake 变量）：

| 落点 | 要改的内容 |
|---|---|
| `vcpkg.json` | `"version"` 字段 |
| `README.md` / `README_zh.md` | 徽章 `version-<v>-lightgrey` / `Version-<v>-lightgrey` |
| `BUILD.md` §13 | 引用根 CMake 的示例片段 `set(AQUA_VERSION "<v>")` |
| 本文档 §5 | 上面那个"当前 `<v>`" |

## 6. 发布前检查

1. Debug / Release 构建无新增 warning；
2. `ctest --preset windows-x64-debug` 与 release 全绿；
3. CLI `--help` 与 `configuration_reference.md` 一致；
4. 启动日志能确认实际音频格式与 F；
5. UDP advertised endpoint 在真实部署地址（含 NAT / 端口映射）上验证；
6. RT debug log 不得作为生产性能基线；
7. Android 包同时验证 native ABI、`libc++_shared.so` 与 release 打包；
8. 发布产物目录 `release/` 不入版本库。
