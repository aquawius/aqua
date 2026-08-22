cmake_minimum_required(VERSION 4.2)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

###################################### VERSIONS ########################################
set(AQUA_CORE_VERSION "0.0.1")
set(AQUA_SERVER_CLI_VERSION "0.0.1")
set(AQUA_CLIENT_CLI_VERSION "0.0.1")
set(AQUA_CLIENT_ANDROID_VERSION "0.0.1")

###################################### PROJECT ########################################
project(aqua VERSION "${AQUA_CORE_VERSION}" LANGUAGES CXX)

if (MSVC)
    add_compile_options(/utf-8 /W4)
else ()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()

# 生成版本头文件
#   - <build>/core/version.h  --> 仅 aqua_core 版本
#   - <build>/app/cli/cli_version.h  -->  server/client CLI 版本
#
# 两者经 include 路径 ${CMAKE_BINARY_DIR} 可见, 前者由 aqua_core 的 PUBLIC include 路径携带, 后者由链接 aqua_core 的 CLI 目标继承同一路径.
# Android App 版本不生成 C++ 头, 由Android/app/build.gradle.kts 直接从本文件读取AQUA_CLIENT_ANDROID_VERSION.
configure_file(src/core/version.h.in
        core/version.h
        @ONLY
)

configure_file(
        src/app/cli/cli_version.h.in
        app/cli/cli_version.h
        @ONLY
)

###################################### OPTIONS ########################################
option(AQUA_BUILD_TEST "Build tests" ON)
option(AQUA_DEBUG "Debug mode, default log level to Debug" OFF)


###################################### FIND PACKAGE ########################################
### Find all deps. All dependencies are managed by vcpkg.
find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)


###################################### RPC/PROTOBUF GENERATE ########################################
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

###################################### SOURCES/TARGET ########################################

set(AQUA_CORE_SOURCES
        src/core/logger/logger.cpp
        src/core/net/address/address_utils.cpp
        src/core/net/grpc/grpc_server.cpp
        src/core/net/grpc/grpc_client.cpp
        src/core/net/grpc/grpc_audio_format_converter.cpp
        src/core/net/transport/udp_socket_base.cpp
        src/core/net/transport/udp_server.cpp
        src/core/net/transport/udp_client.cpp
)

# WASAPI 后端仅 Windows 编译
if (WIN32)
    list(APPEND AQUA_CORE_SOURCES
    )
endif ()

add_library(aqua_core STATIC
        ${AQUA_CORE_SOURCES}
)

target_include_directories(aqua_core
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(aqua_core PUBLIC
        asio::asio
        spdlog::spdlog
        aqua_proto
)

if (WIN32)
    target_compile_definitions(aqua_core PUBLIC
            _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )

    target_link_libraries(aqua_core PUBLIC
            ole32    # COM support
            winmm    # WASAPI support
            ws2_32   # Winsock support
    )
endif ()

if (AQUA_DEBUG)
    target_compile_definitions(aqua_core PUBLIC AQUA_DEBUG)
endif ()

###################################### TEST ########################################
if (AQUA_BUILD_TEST)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif ()
