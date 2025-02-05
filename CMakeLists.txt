####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua_client
        VERSION 0.0.1
)

# c++ standard 23 for std::print support
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED 23)

# 位置无关代码, -fPIC
#set(CMAKE_POSITION_INDEPENDENT_CODE ON)

####################### prepare #######################

# aqua_client_BINARY_NAME
set(aqua_client_BINARY_NAME "aqua_client")

# aqua_client_PLATFORM_NAME
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(aqua_client_PLATFORM_NAME "windows")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(aqua_client_PLATFORM_NAME "linux")
endif ()

if (WIN32)
    add_compile_definitions(_UNICODE UNICODE)
endif ()

message(STATUS "aqua_client_PLATFORM_NAME ${aqua_client_PLATFORM_NAME}")
message(STATUS "aqua_client_BINARY_NAME ${aqua_client_BINARY_NAME}")

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

add_executable(aqua_client
        src/main.cpp
        src/network_client.cpp
        src/network_client.h
        src/signal_handler.cpp
        src/signal_handler.h
        src/rpc_client.cpp
        src/rpc_client.h
        src/linux/audio_playback_linux.cpp
        src/linux/audio_playback_linux.h
        src/adaptive_buffer.cpp
        src/adaptive_buffer.h
        src/cmdline_parser.cpp
        src/cmdline_parser.h
)

####################### link #######################
target_link_libraries(aqua_client PRIVATE
        proto_lib
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt
        protobuf::libprotobuf
        gRPC::grpc++
)

# link with pipewire
if (${aqua_client_PLATFORM_NAME} STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(aqua_client PRIVATE PkgConfig::pipewire)
endif ()

####################### tester_build #######################

add_executable(test_client_main
        test/test_client_main.cpp
        test/rpc_client/rpc_test_client.h
        test/rpc_client/rpc_test_client.cpp
        test/udp_client/udp_test_client.h
        test/udp_client/udp_test_client.cpp
)

target_link_libraries(test_client_main PRIVATE
        proto_lib
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt
        protobuf::libprotobuf
        gRPC::grpc++
)

add_executable(rpc_test_client
        test/rpc_client/rpc_test_client.cpp
        test/rpc_client/rpc_test_client.h
        test/rpc_client/rpc_test_client_main.cpp
)

target_link_libraries(rpc_test_client PRIVATE
        proto_lib
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt
        protobuf::libprotobuf
        gRPC::grpc++
)

add_executable(udp_test_client
        test/udp_client/udp_test_client.cpp
        test/udp_client/udp_test_client.h
        test/udp_client/udp_test_client_main.cpp
)

target_link_libraries(udp_test_client PRIVATE
        proto_lib
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt
        protobuf::libprotobuf
        gRPC::grpc++
)