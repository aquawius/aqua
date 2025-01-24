####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua_core
        VERSION 0.0.1
)

# c++ standard 23 for std::print support
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED 23)

# 位置无关代码, -fPIC     TODO: figure out.
#set(CMAKE_POSITION_INDEPENDENT_CODE ON)

####################### prepare #######################

# aqua_core_BINARY_NAME
set(aqua_core_BINARY_NAME "aqua")

# aqua_core_PLATFORM_NAME
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(aqua_core_PLATFORM_NAME "windows")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(aqua_core_PLATFORM_NAME "linux")
endif ()

if (WIN32)
    add_compile_definitions(_UNICODE UNICODE)
endif ()

message(STATUS "aqua_core_PLATFORM_NAME ${aqua_core_PLATFORM_NAME}")
message(STATUS "aqua_core_BINARY_NAME ${aqua_core_BINARY_NAME}")

# version info generate
message(STATUS "creating configure_file()")
configure_file("${PROJECT_SOURCE_DIR}/src/config.h.in" "${PROJECT_SOURCE_DIR}/src/config.h")

# should use these library.
message(STATUS "Finding libraries which required.")
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

## 这块写的有点丑，但是我不知道怎样改，那两个简单的函数cmake找不到
# 定义路径
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES "${PROTO_PATH}/audio_service.proto")
set(GENERATED_PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proto_gen")
file(MAKE_DIRECTORY ${GENERATED_PROTO_PATH})

# 查找 Protobuf 和 gRPC 编译插件
find_program(PROTOC_EXECUTABLE protoc)
if (NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "Cannot find protoc compiler!")
endif ()

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if (NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "Cannot find grpc_cpp_plugin!")
endif ()

# 生成 Protobuf 文件
add_custom_command(
        OUTPUT ${GENERATED_PROTO_PATH}/audio_service.pb.cc ${GENERATED_PROTO_PATH}/audio_service.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --cpp_out=${GENERATED_PROTO_PATH}
        -I ${PROTO_PATH}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
        COMMENT "Generating Protobuf code"
)

# 生成 gRPC 文件
add_custom_command(
        OUTPUT ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.cc ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --grpc_out=${GENERATED_PROTO_PATH}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        -I ${PROTO_PATH}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
        COMMENT "Generating gRPC code"
)

# 包含生成路径
include_directories("${GENERATED_PROTO_PATH}")

# 设置源文件和头文件列表
set(PROTO_SRCS
        ${GENERATED_PROTO_PATH}/audio_service.pb.cc
        ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.cc
)
set(PROTO_HDRS
        ${GENERATED_PROTO_PATH}/audio_service.pb.h
        ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.h
)

# 添加生成文件的库
add_library(proto_lib
        ${PROTO_SRCS}
        ${PROTO_HDRS}
)

####################### compile #######################
include_directories("${PROJECT_SOURCE_DIR}/src")

add_executable(aqua_core src/main.cpp
        src/cmdline_parser.cpp
        src/cmdline_parser.h
        src/network_server.cpp
        src/network_server.h
        src/audio_manager.cpp
        src/audio_manager.h
        src/linux/audio_manager_impl_linux.cpp
        src/linux/audio_manager_impl_linux.h
        src/windows/audio_manager_impl_windows.cpp
        src/windows/audio_manager_impl_windows.h
        src/signal_handler.cpp
        src/signal_handler.h
        src/session_manager.cpp
        src/session_manager.h
        src/session.hpp
        src/rpc_server.cpp
        src/rpc_server.h
        src/formatter.hpp
)

####################### link #######################
target_link_libraries(aqua_core PRIVATE
        proto_lib
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt
        protobuf::libprotobuf
        gRPC::grpc++
)

# link with pipewire
if (${aqua_core_PLATFORM_NAME} STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(aqua_core PRIVATE PkgConfig::pipewire)
endif ()