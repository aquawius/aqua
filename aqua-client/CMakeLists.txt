####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua-client
        VERSION 0.2
        LANGUAGES CXX
)

set(TARGET_NAME aqua_client)

# aqua_client_VERSION
set(aqua_client_VERSION ${PROJECT_VERSION})

# aqua_client_BINARY_NAME
set(aqua_client_BINARY_NAME "aqua_client")

# aqua_client_PLATFORM_NAME
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(aqua_client_PLATFORM_NAME "windows")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(aqua_client_PLATFORM_NAME "linux")
endif ()

# version info generate
message(STATUS "creating configure_file()")
configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.h"
)

####################### compile #######################
add_executable(${TARGET_NAME}
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
        src/windows/audio_playback_windows.cpp
        src/windows/audio_playback_windows.h
        src/audio_playback.cpp
        src/audio_playback.h
)

# 自动包含父项目生成的proto路径
target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}/proto_gen
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}  # 生成的version.h路径
)

####################### link #######################
target_link_libraries(${TARGET_NAME} PRIVATE
        aqua_proto
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
)

# link with pipewire
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(${TARGET_NAME} PRIVATE PkgConfig::pipewire)
endif()