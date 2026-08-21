# 测试目录按模块分层：
#   core/ — 核心库（logger/session/audio/net/grpc/jitter/diagnostics）单元与集成测试
#   cli/  — CLI 前端（cli_parser_server / cli_parser_client）测试
set(TEST_SOURCES
    core/test_core_logger.cpp
)

add_executable(aqua_tests ${TEST_SOURCES})

target_link_libraries(aqua_tests PRIVATE
        aqua_core
        aqua_proto
        cxxopts::cxxopts
        GTest::gtest
        GTest::gmock
        GTest::gtest_main
)

target_include_directories(aqua_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
