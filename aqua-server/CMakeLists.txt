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

####################### public library #######################
set(COMMON_SOURCES
        src/network_server.cpp
        src/audio_manager.cpp
        src/session_manager.cpp
        src/rpc_server.cpp
        src/session.hpp
        src/formatter.hpp
)

set(PLATFORM_SOURCES
        $<IF:$<PLATFORM_ID:Linux>,
        src/linux/audio_manager_impl_linux.cpp,
        src/windows/audio_manager_impl_windows.cpp>
)

add_library(${TARGET_NAME}_common STATIC
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
)

target_include_directories(${TARGET_NAME}_common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src          # 原始头文件路径
        ${CMAKE_CURRENT_BINARY_DIR}              # version.h
        ${CMAKE_BINARY_DIR}/proto_gen            # Proto生成头文件
)

target_link_libraries(${TARGET_NAME}_common PUBLIC
        aqua_proto
        Boost::system
        spdlog::spdlog
        cxxopts::cxxopts
)

# link with pipewire
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(${TARGET_NAME}_common PRIVATE PkgConfig::pipewire)
endif ()

####################### compile #######################
add_executable(${TARGET_NAME}
        src/main.cpp
        src/cmdline_parser.cpp
        src/signal_handler.cpp
)

# POSITION_INDEPENDENT_CODE
set_target_properties(${TARGET_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ${BUILD_SHARED_LIBS}
)

####################### link #######################
target_link_libraries(${TARGET_NAME} PRIVATE
        aqua_server_common  # 链接共享代码库
)