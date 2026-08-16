cmake_minimum_required(VERSION 4.2)
project(aqua VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)
option(AQUA_DEBUG "Default log level to Debug (defines AQUA_DEBUG macro)" OFF)

if (MSVC)
    add_compile_options(/utf-8 /W4)
    # C4324: struct padded due to alignas — expected for SPSC ringbuffer cache-line alignment
    add_compile_options(/wd4324)
else ()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()

### Find all deps. All dependencies are managed by vcpkg.
find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)


### Protobuf/gRPC generate file stage, add to library.
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES "${PROTO_PATH}/aqua_service.proto")

add_library(aqua_proto STATIC ${PROTO_FILES})
target_link_libraries(aqua_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

# protobuf_generate places generated files under CMAKE_CURRENT_BINARY_DIR,
# mirroring the source directory structure.
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

### aqua_core
set(AQUA_CORE_SOURCES
        src/core/logger/logger.cpp
        src/core/session/session_manager.cpp
        src/core/audio/ringbuffer/spsc_ringbuffer.cpp
        src/core/jitter_buffer/jitter_buffer.cpp
        src/core/diagnostics/diagnostics_manager.cpp
        src/core/net/transport/udp_transport.cpp
        src/core/net/packet/packet.cpp
        src/core/audio/backend/audio_backend_factory.cpp
        src/core/grpc/audio_format_converter.cpp
        src/core/grpc/grpc_server.cpp
        src/core/grpc/grpc_client.cpp
        src/core/server/server_runtime.cpp
        src/core/client/client_runtime.cpp
)

# WASAPI 后端仅 Windows 编译
if (WIN32)
    list(APPEND AQUA_CORE_SOURCES
            src/core/audio/backend/wasapi/wasapi_capture.cpp
            src/core/audio/backend/wasapi/wasapi_playback.cpp
    )
endif ()

# AAudio 后端仅 Android 编译
if (ANDROID)
    list(APPEND AQUA_CORE_SOURCES
            src/core/audio/backend/aaudio/aaudio_playback.cpp
    )
endif ()

add_library(aqua_core STATIC
        ${AQUA_CORE_SOURCES}
)

target_include_directories(aqua_core
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(aqua_core PUBLIC
        asio::asio
        spdlog::spdlog
        aqua_proto
)

### aqua_capi（C ABI 包装层：包 ServerRuntime / ClientRuntime，供 Qt6 / Kotlin+JNI 接入）
# Android 上编成共享库 libaqua.so（含 C API + 后续 JNI），由 App 经 System.loadLibrary 加载。
if (ANDROID)
    add_library(aqua_capi SHARED
            src/core/capi/aqua_capi.cpp
    )
    set_target_properties(aqua_capi PROPERTIES OUTPUT_NAME "aqua")
    # 隐藏内部符号，只显式导出 AQUA_API 标记的 C API（见 include/aqua.h）。
    set_target_properties(aqua_capi PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    target_compile_definitions(aqua_capi PRIVATE AQUA_BUILD_SHARED)
else ()
    add_library(aqua_capi STATIC
            src/core/capi/aqua_capi.cpp
    )
endif ()

target_include_directories(aqua_capi PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aqua_capi PUBLIC
        aqua_core
)

### CLI executables (frontend layer, depends on aqua_core)
set(AQUA_SERVER_SOURCES
        src/app/cli/cli_parser_server.cpp
        src/app/cli/server_main.cpp
)

set(AQUA_CLIENT_SOURCES
        src/app/cli/cli_parser_client.cpp
        src/app/cli/client_main.cpp
)

# Android 上不构建 CLI 可执行文件：native 以 libaqua.so 形式被 App 通过 JNI 加载，
# 命令行入口无意义。CLI 解析器仍在 AQUA_SERVER_SOURCES/AQUA_CLIENT_SOURCES 中定义，
# 桌面平台照常构建。
if (NOT ANDROID)
    add_executable(aqua_server ${AQUA_SERVER_SOURCES})
    target_link_libraries(aqua_server PRIVATE aqua_core cxxopts::cxxopts)

    add_executable(aqua_client ${AQUA_CLIENT_SOURCES})
    target_link_libraries(aqua_client PRIVATE aqua_core cxxopts::cxxopts)
endif ()

if (WIN32)
    target_compile_definitions(aqua_core PUBLIC
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )

    target_link_libraries(aqua_core PUBLIC
            ole32
            winmm
            ws2_32
    )
endif ()

if (ANDROID)
    target_link_libraries(aqua_core PUBLIC
            aaudio
    )
endif ()

if (AQUA_DEBUG)
    target_compile_definitions(aqua_core PUBLIC AQUA_DEBUG)
endif ()

if (BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif ()
