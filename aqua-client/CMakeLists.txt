####################### start #######################
cmake_minimum_required(VERSION 3.27)
project(aqua-client
        VERSION 0.4
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

####################### public library #######################
set(COMMON_SOURCES
        src/network_client.cpp
        src/audio_playback.cpp
        src/rpc_client.cpp
        src/adaptive_buffer.cpp
        src/cmdline_parser.cpp
        src/formatter.hpp
        src/audio_format_common.hpp
)

set(PLATFORM_SOURCES
        $<IF:$<PLATFORM_ID:Linux>,
        src/linux/audio_playback_linux.cpp,
        src/windows/audio_playback_windows.cpp>
)

add_library(${TARGET_NAME}_common STATIC
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
)

target_include_directories(${TARGET_NAME}_common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src          # 原始头文件路径
        ${CMAKE_CURRENT_BINARY_DIR}              # version.h
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
        aqua_client_common  # 链接共享代码库
)

# 安装公共库
install(TARGETS ${TARGET_NAME}_common
        EXPORT aquaTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include
)

# 安装可执行文件
install(TARGETS ${TARGET_NAME}
        RUNTIME DESTINATION bin
)