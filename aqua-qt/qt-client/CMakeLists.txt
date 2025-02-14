cmake_minimum_required(VERSION 3.30)
project(aqua-qt-client LANGUAGES CXX)

file(GLOB UI_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.ui
)

add_executable(aqua-qt-client
        src/main.cpp
        ${UI_FILES}
)

target_include_directories(aqua-qt-client PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/ui
)

target_link_libraries(aqua-qt-client PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network

        aqua_proto
        spdlog::spdlog
)