cmake_minimum_required(VERSION 3.30)
project(aqua-qt LANGUAGES CXX)

# 必须启用Qt的元对象编译器（MOC）
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_PREFIX_PATH "/home/aquawius/Qt/6.8.2/gcc_64")

find_package(Qt6 COMPONENTS Core Widgets Gui Network REQUIRED)

# 添加子模块
if (BUILD_QT_CLIENT)
    add_subdirectory(qt-client)
endif ()

if (BUILD_QT_SERVER)
    add_subdirectory(qt-server)
endif ()