####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua-qt-server VERSION 0.1 LANGUAGES CXX)

set(TARGET_NAME aqua_qt_server)

# aqua_server_VERSION
set(aqua_server_VERSION ${PROJECT_VERSION})

# aqua_server_BINARY_NAME
set(aqua_server_BINARY_NAME ${TARGET_NAME})

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

####################### prepare #######################

# ui files
file(GLOB UI_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.ui
)

add_executable(${TARGET_NAME}
        src/main.cpp
        ${UI_FILES}
        src/ui/ServerMainWindow.cpp
        src/ui/ServerMainWindow.h
        src/ui/ServerMainWindow.ui
)

# POSITION_INDEPENDENT_CODE
set_target_properties(${TARGET_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ${BUILD_SHARED_LIBS}
)

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/ui
        ${CMAKE_BINARY_DIR}/proto_gen
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}  # 生成的version.h路径
)

####################### link #######################
target_link_libraries(${TARGET_NAME} PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network

        aqua_proto
        spdlog::spdlog
)

if (WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(DEBUG_SUFFIX)
    if (MSVC AND CMAKE_BUILD_TYPE MATCHES "Debug")
        set(DEBUG_SUFFIX "d")
    endif ()
    set(QT_INSTALL_PATH "${CMAKE_PREFIX_PATH}")
    if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
        set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        if (NOT EXISTS "${QT_INSTALL_PATH}/bin")
            set(QT_INSTALL_PATH "${QT_INSTALL_PATH}/..")
        endif ()
    endif ()
    if (EXISTS "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/plugins/platforms/")
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/plugins/platforms/qwindows${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/plugins/platforms/")
    endif ()
    foreach (QT_LIB Core Gui Widgets)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                "${QT_INSTALL_PATH}/bin/Qt6${QT_LIB}${DEBUG_SUFFIX}.dll"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>")
    endforeach (QT_LIB)
endif ()

# link with pipewire
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(${TARGET_NAME} PRIVATE PkgConfig::pipewire)
endif ()