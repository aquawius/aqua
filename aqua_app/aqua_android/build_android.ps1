# 构建 Android native 库并同步到 per-buildType jniLibs（根 CMake + vcpkg 交叉编译，Gradle 只打包）。
#
# 前置：ANDROID_NDK_HOME 已设置；vcpkg 已安装 arm64-android 依赖（首次由 preset 触发）。
# 用法（在仓库根目录）：powershell -ExecutionPolicy Bypass -File aqua_app/aqua_android/build_android.ps1
#   -SkipDebug   跳过 debug 构建（只更新 release 库）
#   -SkipRelease 跳过 release 构建（只更新 debug 库）
#
# 产物（AGP 按 buildType 各取一份；libc++_shared.so 共享放 main）：
#   aqua_app/aqua_android/app/src/debug/jniLibs/arm64-v8a/libaqua.so     ← Debug
#   aqua_app/aqua_android/app/src/release/jniLibs/arm64-v8a/libaqua.so   ← Release
#   aqua_app/aqua_android/app/src/main/jniLibs/arm64-v8a/libc++_shared.so
# 之后在 aqua_app/aqua_android 下用 Android Studio 或 .\gradlew.bat assembleDebug / assembleRelease 打包 APK。
#
# 注：debug/release 都会 llvm-strip --strip-debug 去掉 DWARF 调试信息，让 APK 体积大幅缩小；
# 完整符号仍保留在 cmake_build/android-arm64-{debug,release}/bin/libaqua.so，需要行号时离线还原。

param(
    [switch]$SkipDebug,
    [switch]$SkipRelease
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # D:\coding\aqua
$appDir = $PSScriptRoot   # aqua_app\aqua_android

function Build-Native([string]$Preset)
{
    cmake --preset $Preset
    cmake --build "$root/cmake_build/$Preset" --target aqua_capi
}

function Strip-Native([string]$Lib)
{
    if ($env:ANDROID_NDK_HOME)
    {
        $llvmStrip = Join-Path $env:ANDROID_NDK_HOME "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
        if (Test-Path $llvmStrip)
        {
            & $llvmStrip --strip-debug $Lib
        }
        else
        {
            Write-Warning "llvm-strip not found, skipping strip: $llvmStrip"
        }
    }
}

# 1. debug 库 → src/debug/jniLibs（开发装机用，strip 后体积小、安装快）
if (-not $SkipDebug)
{
    Build-Native "android-arm64-debug"
    $dst = Join-Path $appDir "app/src/debug/jniLibs/arm64-v8a"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $lib = Join-Path $dst "libaqua.so"
    Copy-Item (Join-Path $root "cmake_build/android-arm64-debug/bin/libaqua.so") $lib -Force
    Strip-Native $lib
}

# 2. release 库 → src/release/jniLibs（发布 APK 用）
if (-not $SkipRelease)
{
    Build-Native "android-arm64-release"
    $dst = Join-Path $appDir "app/src/release/jniLibs/arm64-v8a"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    $lib = Join-Path $dst "libaqua.so"
    Copy-Item (Join-Path $root "cmake_build/android-arm64-release/bin/libaqua.so") $lib -Force
    Strip-Native $lib
}

# 3. libc++_shared.so → main（两个 buildType 共用；ANDROID_STL=c++_shared 需随 APK 打包）
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk)
{
    throw "ANDROID_NDK_HOME is not set"
}
$cxxShared = Join-Path $ndk "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
$mainLibs = Join-Path $appDir "app/src/main/jniLibs/arm64-v8a"
New-Item -ItemType Directory -Force -Path $mainLibs | Out-Null
if (Test-Path $cxxShared)
{
    Copy-Item $cxxShared $mainLibs -Force
}
else
{
    Write-Warning "libc++_shared.so not found at: $cxxShared"
}

Write-Host "Done. Native libs:"
Get-ChildItem (Join-Path $appDir "app/src") -Recurse -Filter *.so |
        Select-Object FullName, @{ N = 'MB'; E = { [math]::Round($_.Length / 1MB, 1) } }
