cmake_minimum_required(VERSION 4.2)
project(aqua VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)

### Find all deps. All dependencies are managed by vcpkg.
find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# for asio need this value in windows compile.
if (WIN32)
    add_compile_definitions(_UNICODE UNICODE _WIN32_WINNT=0x0A00)
endif ()

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
        src/core/common/logger.cpp
        src/core/session/session_manager.cpp
)

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
        cxxopts::cxxopts
        stduuid
        aqua_proto
)

if (BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif ()