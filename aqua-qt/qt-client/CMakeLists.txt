####################### start #######################
cmake_minimum_required(VERSION 3.27)
project(aqua-qt-client VERSION 0.4 LANGUAGES CXX)

set(TARGET_NAME aqua_qt_client)

# aqua_client_VERSION
set(aqua_client_VERSION ${PROJECT_VERSION})

# aqua_client_BINARY_NAME
set(aqua_client_BINARY_NAME ${TARGET_NAME})

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

####################### prepare #######################

# ui files
file(GLOB UI_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.ui
)
# ui files in /src/ui , so need this to set AUTOUIC find ui files.
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)

if (WIN32)
    add_executable(${TARGET_NAME} WIN32
            src/main.cpp
            ${UI_FILES}
            src/ui/ClientMainWindow.cpp
            src/ui/ClientMainWindow.ui
            src/ui/AudioMeterWidget.cpp
    )
else ()
    add_executable(${TARGET_NAME}
            src/main.cpp
            ${UI_FILES}
            src/ui/ClientMainWindow.cpp
            src/ui/ClientMainWindow.ui
            src/ui/AudioMeterWidget.cpp
    )
endif ()

# POSITION_INDEPENDENT_CODE
set_target_properties(${TARGET_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ${BUILD_SHARED_LIBS}
)

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/ui
        ${CMAKE_BINARY_DIR}/proto_gen
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_BINARY_DIR}  # 生成的version.h路径
)

####################### link #######################
target_link_libraries(${TARGET_NAME} PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets

        aqua_proto
        aqua_client_common
        spdlog::spdlog
)

# link with pipewire
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(pipewire REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(${TARGET_NAME} PRIVATE PkgConfig::pipewire)
endif ()

# 安装Qt服务器可执行文件
install(TARGETS ${TARGET_NAME}
        RUNTIME DESTINATION bin
)

if (WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    if (WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${WINDEPLOYQT_EXECUTABLE} "$<TARGET_FILE:${TARGET_NAME}>"
                COMMENT "Deploying Qt DLLs using windeployqt..."
        )
    else ()
        message(WARNING "windeployqt not found in: ${QT_BIN_DIR}")
    endif ()

    install(CODE "
      message(STATUS \"Running windeployqt on installed aqua_qt_client.exe...\")
      execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" \"${CMAKE_INSTALL_PREFIX}/bin/aqua_qt_client.exe\")
    ")
endif ()

