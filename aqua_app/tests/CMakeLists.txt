# aqua_app 侧测试：CLI 前端与 Core 的契约。
#
# 为什么放在 aqua_app 而不是 aqua_core：被测对象是 CLI 前端
# （cli/cli_parser/cli_parser_client.cpp），它只被两个 CLI 可执行目标编译，不是库。
# 这里是它的第一个测试宿主。
#
# 依赖：CLI parser 实现直接编进本目标（与 aqua_client_cli 重复编译一次，
# 换取"不改动任何现有目标"的零风险）；cxxopts 由父目录的 find_package 提供。
# 只在 AQUA_BUILD_TEST 时由 aqua_app/CMakeLists.txt 引入。

find_package(Threads REQUIRED)

add_executable(aqua_cli_contract_tests
        cli_endpoint_contract_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../cli/cli_parser/cli_parser_client.cpp
)

target_link_libraries(aqua_cli_contract_tests PRIVATE
        aqua_client_core
        cxxopts::cxxopts
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(aqua_cli_contract_tests PRIVATE
        ${CMAKE_BINARY_DIR}                            # <aqua_app/cli/cli_version.h>（生成物）
        ${CMAKE_CURRENT_SOURCE_DIR}/../cli/cli_parser  # cli_parser_client.h / cli_common.h
)

include(GoogleTest)
gtest_discover_tests(aqua_cli_contract_tests)
