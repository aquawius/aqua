cmake_minimum_required(VERSION 4.2)

###################################### VERSIONS ########################################
# 单一版本源 AQUA_VERSION，与根目录 vcpkg.json 的 version 字段保持一致
# （vcpkg.json 是纯字面量、无法引用 CMake 变量，改动时二者需手动同步）。
# 其余版本号（core 版本头 / CLI 版本头 / Android build.gradle.kts）由此派生。
set(AQUA_VERSION "0.2.0")
set(AQUA_CORE_VERSION "${AQUA_VERSION}")
set(AQUA_SERVER_CLI_VERSION "${AQUA_VERSION}")
set(AQUA_CLIENT_CLI_VERSION "${AQUA_VERSION}")
set(AQUA_CLIENT_ANDROID_VERSION "${AQUA_VERSION}") # Android App 由 build.gradle.kts 直接读取本值

###################################### PROJECT ########################################
project(aqua VERSION "${AQUA_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /W4)
else ()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()

###################################### 编译开关 ########################################
# 核心库：分别编译服务端 / 客户端核心（均依赖公共 aqua_core_base）
option(AQUA_BUILD_SERVER_CORE "Build the aqua server core library" ON)
option(AQUA_BUILD_CLIENT_CORE "Build the aqua client core library" ON)

# 测试与调试
option(AQUA_BUILD_TEST "Build tests" ON)
option(AQUA_DEBUG "Enable debug build definitions" OFF)
option(AQUA_JITTER_BUFFER_RT_DEBUG_LOG "Enable synchronous JitterBuffer realtime-path diagnostic logging (development only)" OFF)

# 应用层（CLI 等原生前端，位于 aqua_app/）；Android 等移动端为独立工程（gradle），不参与本 CMake
option(AQUA_BUILD_APPS "Build application targets under aqua_app/" ON)

###################################### 测试基础设施 ########################################
# GTest 与 CTest 由顶层统一准备，各 tests 子目录直接使用 GTest:: 目标。
if (AQUA_BUILD_TEST)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
endif ()

###################################### CORE ########################################
# 核心库构建逻辑（依赖查找 / proto 生成 / 版本头 / 三核心目标 / 测试）自包含于
# aqua_core/CMakeLists.txt。
add_subdirectory(aqua_core)

###################################### APPS ########################################
# 应用层（CLI 等原生前端）：构建逻辑自包含于 aqua_app/CMakeLists.txt
#（cli_version 生成、CLI 可执行目标等）。
if (AQUA_BUILD_APPS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/aqua_app/CMakeLists.txt")
    add_subdirectory(aqua_app)
endif ()
