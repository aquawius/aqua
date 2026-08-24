# aqua_core 构建脚本（自包含的核心库）。
#
# 产出静态库目标：
#   aqua_core_base     —— 公共核心：logger / session / address / 音频格式转换 / UDP socket 基类 / 设备枚举
#   aqua_server_core   —— 服务端核心（音频源）：gRPC server / UDP server / 采集 / server runtime
#   aqua_client_core   —— 客户端核心（音频汇）：gRPC client / UDP client / 回放 / client runtime
#
# 采集与回放分属两个程序：server 只编译采集，client 只编译回放；设备枚举两端共用放 base。
# 不要在同一个 core 里同时编入 capture 与 playback。
#
# 目录约定：
#   include/aqua/  公共头（按模块）；src/  实现（按模块；平台后端将来进 src/<模块>/backend/）
#   proto/         协议定义；tests/  测试
# 版本头由 include/aqua/version.h.in 生成到 ${CMAKE_BINARY_DIR}/include/aqua/version.h，
# 经各核心目标的 PUBLIC include 路径对外可见（#include <aqua/version.h>）。

find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# ---- 版本头（模板放公共头目录，生成物镜像到构建目录）----
configure_file(include/aqua/version.h.in
        ${CMAKE_BINARY_DIR}/include/aqua/version.h
        @ONLY
)

# ---- proto / gRPC 生成 ----
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES "${PROTO_PATH}/aqua_service.proto")

add_library(aqua_proto STATIC ${PROTO_FILES})
target_link_libraries(aqua_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

# protobuf_generate 将生成文件放在 CMAKE_CURRENT_BINARY_DIR 下，
# 镜像源码目录结构。
target_include_directories(aqua_proto PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/proto
)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET aqua_proto LANGUAGE cpp)
protobuf_generate(TARGET aqua_proto LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
)

# ---- 公共核心：两端共享的领域与基础设施 ----
add_library(aqua_core_base STATIC
        src/logger/logger.cpp
        src/session/session_manager.cpp
        src/net/address/address_utils.cpp
        src/audio/audio_format_converter.cpp
        src/audio/devices/audio_device_manager_factory.cpp
        src/net/udp/udp_socket_base.cpp
)

if (WIN32)
    target_sources(aqua_core_base PRIVATE
            src/audio/devices/wasapi/wasapi_device_manager.cpp
    )
endif ()

# ---- 服务端核心（音频源：采集 -> UDP 广播；开关 AQUA_BUILD_SERVER_CORE）----
if (AQUA_BUILD_SERVER_CORE)
    add_library(aqua_server_core STATIC
            src/net/grpc/grpc_server.cpp
            src/net/udp/udp_server.cpp
            src/audio/capture/audio_capture_factory.cpp
            src/runtime/server_runtime.cpp
    )
    if (WIN32)
        target_sources(aqua_server_core PRIVATE
                src/audio/capture/wasapi/wasapi_audio_capture.cpp
        )
    endif ()
    target_link_libraries(aqua_server_core PUBLIC aqua_core_base)
endif ()

# ---- 客户端核心（音频汇：UDP 接收 -> 回放；开关 AQUA_BUILD_CLIENT_CORE）----
if (AQUA_BUILD_CLIENT_CORE)
    add_library(aqua_client_core STATIC
            src/net/grpc/grpc_client.cpp
            src/net/udp/udp_client.cpp
            src/audio/playback/audio_playback_factory.cpp
            src/runtime/client_runtime.cpp
    )
    if (WIN32)
        target_sources(aqua_client_core PRIVATE
                src/audio/playback/wasapi/wasapi_audio_playback.cpp
        )
    endif ()
    target_link_libraries(aqua_client_core PUBLIC aqua_core_base)
endif ()

# ---- 公共编译设置（三个核心目标共用）----
foreach (core_target IN ITEMS aqua_core_base aqua_server_core aqua_client_core)
    if (NOT TARGET ${core_target})
        continue()
    endif ()

    target_include_directories(${core_target} PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/include   # 生成头（aqua/version.h）
    )
    target_include_directories(${core_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src   # platform/backend internal headers
    )
    target_link_libraries(${core_target} PUBLIC
            asio::asio
            spdlog::spdlog
            aqua_proto
    )
    if (WIN32)
        target_compile_definitions(${core_target} PUBLIC
                _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
        )
        target_link_libraries(${core_target} PUBLIC
                ole32    # COM support
                avrt     # MMCSS audio thread support
                winmm    # Windows multimedia support
                ws2_32   # Winsock support
        )
    endif ()
    if (AQUA_DEBUG)
        target_compile_definitions(${core_target} PUBLIC AQUA_DEBUG)
    endif ()
endforeach ()

# WASAPI Device backend 已按平台加入 aqua_core_base；Capture backend 进入 server core。
# src/audio/wasapi/wasapi_com.h 是 Windows Audio backends 共用的 private COM-apartment RAII helper。

# ---- 测试 ----
if (AQUA_BUILD_TEST)
    # net 测试同时使用 server 与 client 两端，二者缺一则跳过测试。
    if (AQUA_BUILD_SERVER_CORE AND AQUA_BUILD_CLIENT_CORE)
        add_subdirectory(tests)
    else ()
        message(WARNING
                "AQUA_BUILD_TEST is ON but a core is disabled (server/client); tests skipped")
    endif ()
endif ()
