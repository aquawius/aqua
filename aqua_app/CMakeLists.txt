# aqua_app：应用层（CLI / Android 等前端）。
# 由根 CMakeLists 的 AQUA_BUILD_APPS 开关控制是否构建。
#
# 目录约定：
#   cli/ —— 命令行前端（产出 aqua_server_cli / aqua_client_cli 两个可执行文件）
#   Android 等移动端由独立工程（gradle 等）构建，不参与本 CMake。

find_package(cxxopts CONFIG REQUIRED)

# CLI 版本头：模板在 cli/cli_version.h.in，生成到构建目录的镜像路径。
# 各 CLI 目标把 ${CMAKE_BINARY_DIR} 加入 include 路径，
# 即可 #include <aqua_app/cli/cli_version.h>。
configure_file(cli/cli_version.h.in
        ${CMAKE_BINARY_DIR}/aqua_app/cli/cli_version.h
        @ONLY
)

# ---- CLI 前端 ----

add_executable(aqua_server_cli
        cli/server_main.cpp
        cli/cli_parser/cli_parser_server.cpp
)
target_link_libraries(aqua_server_cli PRIVATE
        aqua_server_core
        cxxopts::cxxopts
)
target_include_directories(aqua_server_cli PRIVATE
        ${CMAKE_BINARY_DIR}
)

add_executable(aqua_client_cli
        cli/client_main.cpp
        cli/cli_parser/cli_parser_client.cpp
)
target_link_libraries(aqua_client_cli PRIVATE
        aqua_client_core
        cxxopts::cxxopts
)
target_include_directories(aqua_client_cli PRIVATE
        ${CMAKE_BINARY_DIR}
)

# ---- Android native 边界 ----
# aqua_capi 共享库（C API + 监督线程）：Android 交叉编译出 libaqua.so，
# Windows host 出 aqua.dll 并可跑冒烟测试。CLI 与其互不影响。
if (AQUA_BUILD_ANDROID AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/native/CMakeLists.txt")
    add_subdirectory(android/native)
endif ()
