cmake_minimum_required(VERSION 3.30)
project(aqua-qt-server LANGUAGES CXX)

file(GLOB UI_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.ui
)

add_executable(aqua-qt-server
        src/main.cpp
        ${UI_FILES}
)

target_include_directories(aqua-qt-server PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/ui
)

target_link_libraries(aqua-qt-server PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network

        aqua_proto
        spdlog::spdlog
)