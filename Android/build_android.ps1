# 构建 Android native 库并同步到 app jniLibs（方案 A：根 CMake + vcpkg 交叉编译，Gradle 只打包）。
#
# 前置：ANDROID_NDK_HOME 已设置；vcpkg 已安装 arm64-android 依赖（首次由 preset 触发）。
# 用法（在项目根目录）：powershell -ExecutionPolicy Bypass -File Android/build_android.ps1
#
# 产物：Android/app/src/main/jniLibs/arm64-v8a/{libaqua.so, libc++_shared.so}
# 之后用 Android Studio 或 ./gradlew assembleDebug 打包 APK。

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot   # D:\coding\aqua

# 1. 配置 + 构建 libaqua.so（首次会触发 vcpkg manifest install for arm64-android）
cmake --preset android-arm64-debug
cmake --build "$root/cmake_build/android-arm64-debug" --target aqua_capi

# 2. 同步到 app jniLibs（arm64-v8a）
$jniLibs = Join-Path $root "Android/app/src/main/jniLibs/arm64-v8a"
New-Item -ItemType Directory -Force -Path $jniLibs | Out-Null
Copy-Item (Join-Path $root "cmake_build/android-arm64-debug/libaqua.so") $jniLibs -Force

# 3. 同步 libc++_shared.so（ANDROID_STL=c++_shared；prebuilt .so 需随 APK 一起打包）
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) {
    throw "ANDROID_NDK_HOME is not set"
}
$cxxShared = Join-Path $ndk "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
if (Test-Path $cxxShared) {
    Copy-Item $cxxShared $jniLibs -Force
} else {
    Write-Warning "libc++_shared.so not found at: $cxxShared"
}

Write-Host "Done. Native libs synced to: $jniLibs"
Get-ChildItem $jniLibs | Select-Object Name, Length
