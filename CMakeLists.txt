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

# 将生成的config.h放到src目录下
message(STATUS "creating configure_file()")
configure_file("${PROJECT_SOURCE_DIR}/src/config.h.in" "${PROJECT_SOURCE_DIR}/src/config.h")

# should use these library.
#find_package(asio REQUIRED)
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
find_package(fmt REQUIRED)


####################### compile #######################
include_directories("${PROJECT_SOURCE_DIR}/src")

add_executable(aqua_core src/main.cpp
        src/uncomplete_functions.hpp
        src/cmdline_parser.cpp
        src/cmdline_parser.h
        src/network_manager.cpp
        src/network_manager.h
        src/audio_manager.cpp
        src/audio_manager.h
        src/linux/audio_manager_impl_linux.cpp
        src/linux/audio_manager_impl_linux.h
        src/windows/audio_manager_impl_windows.cpp
        src/windows/audio_manager_impl_windows.h
)

####################### link #######################
target_link_libraries(aqua_core PRIVATE
        spdlog::spdlog
        cxxopts::cxxopts
        fmt::fmt)

# link with pipewire
if (${aqua_core_PLATFORM_NAME} STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(aqua_core PRIVATE PkgConfig::pipewire)
endif ()
