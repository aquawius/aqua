# aqua_core 构建脚本（自包含的核心库）。
#
# 产出静态库目标：
#   aqua_core_base     —— 公共核心：logger / session / address / UDP 传输 / 设备枚举
#   aqua_server_core   —— 服务端核心（音频源）：gRPC server / UDP server（数据面协议）/ 采集 / server runtime
#   aqua_client_core   —— 客户端核心（音频汇）：gRPC client / UDP client（数据面协议）/ 回放 / client runtime
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

# audio_format_converter（audio::AudioFormat <-> pb::AudioFormat）只在 gRPC 边界被
# server/client 使用，归属 proto 边界，放这里而非 aqua_core_base，避免 base 依赖 proto。
add_library(aqua_proto STATIC
        ${PROTO_FILES}
        src/audio/audio_format_converter.cpp
)
target_link_libraries(aqua_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
        spdlog::spdlog
)

# protobuf_generate 将生成文件放在 CMAKE_CURRENT_BINARY_DIR 下，
# 镜像源码目录结构。audio_format_converter 需要原生 audio 头（aqua/audio/audio_format.h）。
target_include_directories(aqua_proto PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/proto
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

protobuf_generate(TARGET aqua_proto LANGUAGE cpp)
protobuf_generate(TARGET aqua_proto LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
)

# ---- 公共核心：两端共享的领域与基础设施 ----
add_library(aqua_core_base STATIC
        src/logger/logger.cpp
        src/diagnostics/diagnostics.cpp
        src/session/session_manager.cpp
        src/net/address/address_utils.cpp
        src/audio/devices/audio_device_manager_factory.cpp
        src/net/udp/udp_transport.cpp
        src/net/udp/network_frame.cpp
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
            src/audio/packetizer/audio_packetizer.cpp
            src/runtime/audio_network_dispatcher.cpp
            src/runtime/server_runtime.cpp
    )
    if (WIN32)
        target_sources(aqua_server_core PRIVATE
                src/audio/capture/wasapi/wasapi_audio_capture.cpp
        )
    endif ()
    target_link_libraries(aqua_server_core PUBLIC aqua_core_base aqua_proto)
endif ()

# ---- 客户端核心（音频汇：UDP 接收 -> 回放；开关 AQUA_BUILD_CLIENT_CORE）----
if (AQUA_BUILD_CLIENT_CORE)
    add_library(aqua_client_core STATIC
            src/net/grpc/grpc_client.cpp
            src/net/udp/udp_client.cpp
            src/audio/playback/audio_playback_factory.cpp
            src/audio/buffer/jitter_buffer.cpp
            src/runtime/client_runtime.cpp
    )
    if (WIN32)
        target_sources(aqua_client_core PRIVATE
                src/audio/playback/wasapi/wasapi_audio_playback.cpp
        )
    endif ()
    target_link_libraries(aqua_client_core PUBLIC aqua_core_base aqua_proto)
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
    if (AQUA_JITTER_BUFFER_RT_DEBUG_LOG AND core_target STREQUAL "aqua_client_core")
        target_compile_definitions(${core_target} PRIVATE AQUA_JITTER_BUFFER_RT_DEBUG_LOG=1)
    endif ()
endforeach ()

# WASAPI Device backend 已按平台加入 aqua_core_base；Capture backend 进入 server core。
# src/audio/wasapi/wasapi_com.h 是 Windows Audio backends 共用的 private COM-apartment RAII helper。

# ---- C API 共享库（client 侧稳定 C 边界，供 JNI / GUI 前端）----
# 产物名统一为 aqua（libaqua.so / aqua.dll）；Android 交叉编译后拷贝进
# app 工程 jniLibs，Windows host 可构建 DLL 并运行冒烟测试。
if (AQUA_BUILD_C_API)
    add_library(aqua_capi SHARED
            src/c_api/aqua_capi.cpp
    )
    target_link_libraries(aqua_capi PRIVATE
            aqua_client_core
    )
    # 头文件经 INTERFACE 传播给消费者（JNI 侧 / 测试）。
    target_include_directories(aqua_capi PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    set_target_properties(aqua_capi PROPERTIES
            OUTPUT_NAME aqua
            # 产物固定到 <build>/bin（含配置子目录）：host 侧与测试 exe 同目录，
            # 依赖 DLL 由 vcpkg applocal 就地拷贝；Android 侧为
            # cmake_build/<preset>/bin/libaqua.so，供同步脚本直接取用。
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    if (WIN32)
        # C 接口无 __declspec(dllexport) 注解，按符号全导出（仅 C 函数，安全）。
        set_target_properties(aqua_capi PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif ()
    if (ANDROID)
        # AAudio playback backend + logcat（spdlog android sink 需要 liblog）。
        target_link_libraries(aqua_capi PRIVATE aaudio log)
    endif ()
endif ()

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
