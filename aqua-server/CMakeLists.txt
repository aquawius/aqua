####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua-server
        VERSION 0.2
        LANGUAGES CXX
)

set(TARGET_NAME aqua_server)

# aqua_server_VERSION
set(aqua_server_VERSION ${PROJECT_VERSION})

# aqua_server_BINARY_NAME
set(aqua_server_BINARY_NAME "aqua_server")

# aqua_server_PLATFORM_NAME
if (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(aqua_server_PLATFORM_NAME "windows")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(aqua_server_PLATFORM_NAME "linux")
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
        src/cmdline_parser.cpp
        src/cmdline_parser.h
        src/network_server.cpp
        src/network_server.h
        src/linux/audio_manager_impl_linux.cpp
        src/linux/audio_manager_impl_linux.h
        src/windows/audio_manager_impl_windows.cpp
        src/windows/audio_manager_impl_windows.h
        src/audio_manager.cpp
        src/audio_manager.h
        src/signal_handler.cpp
        src/signal_handler.h
        src/session_manager.cpp
        src/session_manager.h
        src/session.hpp
        src/rpc_server.cpp
        src/rpc_server.h
        src/formatter.hpp
)

# 自动包含父项目生成的proto路径
target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}/proto_gen
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}  # 生成的version.h路径
)

# POSITION_INDEPENDENT_CODE
set_target_properties(${TARGET_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ${BUILD_SHARED_LIBS}
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