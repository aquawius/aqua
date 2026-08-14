cmake_minimum_required(VERSION 4.2)
project(aqua VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)
option(AQUA_DEBUG "Default log level to Debug (defines AQUA_DEBUG macro)" OFF)

if (MSVC)
    add_compile_options(/utf-8)
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
)

# WASAPI 后端仅 Windows 编译
if (WIN32)
    list(APPEND AQUA_CORE_SOURCES
            src/core/audio/backend/wasapi/wasapi_capture.cpp
            src/core/audio/backend/wasapi/wasapi_playback.cpp
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

### CLI executables (frontend layer, depends on aqua_core)
set(AQUA_SERVER_SOURCES
        src/app/cli/cli_parser_server.cpp
        src/app/cli/server_main.cpp
)

set(AQUA_CLIENT_SOURCES
        src/app/cli/cli_parser_client.cpp
        src/app/cli/client_main.cpp
)

add_executable(aqua_server ${AQUA_SERVER_SOURCES})
target_link_libraries(aqua_server PRIVATE aqua_core cxxopts::cxxopts)

add_executable(aqua_client ${AQUA_CLIENT_SOURCES})
target_link_libraries(aqua_client PRIVATE aqua_core cxxopts::cxxopts)

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

if (AQUA_DEBUG)
    target_compile_definitions(aqua_core PUBLIC AQUA_DEBUG)
endif ()

if (BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif ()
