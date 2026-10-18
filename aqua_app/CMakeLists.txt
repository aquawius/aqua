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

# ---- GUI 前端（Dear ImGui + SDL3）----
# 桌面图形客户端（Windows / Linux / macOS 一套代码；SDL3 platform binding +
# SDLRenderer3 renderer binding，二者由 vcpkg port 编入 imgui 静态库、只装头文件）。
#
# 依赖独立性：imgui / sdl3 只声明在根 vcpkg.json 的 manifest feature "gui" 中，
# 由桌面 preset 的 VCPKG_MANIFEST_FEATURES（"tests;gui"）激活。因此：
#   构建 GUI（默认）：cmake --preset windows-x64-debug
#   不构建 GUI      ：cmake --preset windows-x64-debug -DAQUA_BUILD_GUI=OFF \
#                       -DVCPKG_MANIFEST_FEATURES=tests
#     —— vcpkg 不下载/编译 imgui 与 sdl3，本 target 也不定义。
#     只传 -DAQUA_BUILD_GUI=OFF 而忘记关 feature 时，依赖仍会被安装（仅不构建）。
if (AQUA_BUILD_GUI)
    find_package(imgui CONFIG QUIET)
    find_package(SDL3 CONFIG QUIET)
    if (NOT imgui_FOUND OR NOT SDL3_FOUND)
        message(FATAL_ERROR
                "AQUA_BUILD_GUI=ON 但未找到 imgui/SDL3。二者声明在 vcpkg manifest "
                "feature \"gui\" 中：请确认 VCPKG_MANIFEST_FEATURES 含 \"gui\""
                "（桌面 preset 默认含），或改用 -DAQUA_BUILD_GUI=OFF 跳过 GUI。")
    endif()

    # spdlog 必须在本目录再 find_package 一次：find_package 生成的 IMPORTED
    # target 只在调用它的目录作用域里可见，而 aqua_core 是 aqua_app 的兄弟
    # 目录——aqua_core/CMakeLists.txt 里的 find_package(spdlog) 不会让
    # spdlog::spdlog 在 aqua_app 里可用（不写这一句，配置期报 "target not
    # found"，随后 link 把 spdlog::spdlog 当成字面库名，报 LNK1104）。
    # GUI 直接 include <spdlog/...>（log_sink.cpp），所以这不是多余的显式依赖：
    # 它的 INTERFACE_COMPILE_DEFINITIONS（SPDLOG_SHARED_LIB 等）也必须生效，
    # 否则 TU 会按 header-only 编 spdlog、与动态库符号重复定义（LNK2005）。
    find_package(spdlog CONFIG REQUIRED)

    # 源文件按职责拆分（依赖方向：数据 → 布局/契约 → 各面板 → 编排）：
    #   main.cpp         SDL3/imgui 启动 + 主循环
    #   app_style.cpp    主题与间距度量
    #   log_buffer.cpp   运行日志缓冲
    #   log_sink.cpp     core 日志（spdlog）→ 日志面板的桥
    #   client_session.cpp 会话层：worker 线程跑 ClientRuntime + 快照发布 + 自动重连
    #   jb_presets.cpp   网络预设表（JB 参数单一数据源）
    #   ui_connection.cpp 连接卡片 + toggle_connection
    #   ui_metrics.cpp   指标（卡片简表 + 设置→统计详表）
    #   ui_telemetry.cpp 遥测面板（6 指标 + 60s 水位曲线，连接页下半屏）
    #   ui_log.cpp       运行日志面板
    #   ui_dialogs.cpp   设置（外观/统计/高级 三 tab）/ 关于
    #   ui_menu.cpp      菜单栏
    #   ui_demo.cpp      演示数据（Phase 0）
    #   ui_main.cpp      编排：draw_main_window
    add_executable(aqua_client_gui
            gui/main.cpp
            gui/app_style.cpp
            gui/log_buffer.cpp
            gui/log_sink.cpp
            gui/client_session.cpp
            gui/jb_presets.cpp
            gui/ui_connection.cpp
            gui/ui_metrics.cpp
            gui/ui_telemetry.cpp
            gui/ui_dialogs.cpp
            gui/ui_log.cpp
            gui/ui_menu.cpp
            gui/ui_demo.cpp
            gui/ui_main.cpp
    )
    target_link_libraries(aqua_client_gui PRIVATE
            aqua_client_core
            imgui::imgui
            SDL3::SDL3
            spdlog::spdlog
    )
    # 复用 CLI 版本头（单一版本源 = 根 CMakeLists 的 AQUA_VERSION）。
    target_include_directories(aqua_client_gui PRIVATE
            ${CMAKE_BINARY_DIR}
    )
    # MSVC 的 C4996（fopen 等"不安全"CRT 函数 deprecation）：
    # 本工程要跨 Linux/macOS，不用 MSVC 专有的 fopen_s，
    # 统一以编译定义关闭该告警（仅影响本 target，不污染 core / CLI）。
    if (MSVC)
        target_compile_definitions(aqua_client_gui PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
endif ()

# ---- 测试（仅 AQUA_BUILD_TEST 时）----
# CLI 前端与 Core 的契约测试需要把 CLI parser 实现编进测试目标，因此挂在 aqua_app 侧。
# 内容见 tests/CMakeLists.txt。
if (AQUA_BUILD_TEST)
    add_subdirectory(tests)
endif ()
