# 测试目录按模块分层：
#   core/ — 核心库（logger/session/audio/net/grpc/jitter/diagnostics）单元与集成测试
#   cli/  — CLI 前端（cli_parser_server / cli_parser_client）测试
set(TEST_SOURCES
        core/test_log.cpp
        core/test_config.cpp
        core/test_session_manager.cpp
        core/test_session_lifecycle.cpp
        core/test_audio_format.cpp
        core/test_audio_format_converter.cpp
        core/test_ringbuffer.cpp
        core/test_packet.cpp
        core/test_udp_transport.cpp
        core/test_nat_flow.cpp
        core/test_data_flow.cpp
        core/test_jitter_buffer.cpp
        core/test_diagnostics.cpp
        core/test_end_to_end.cpp
        core/test_concurrency.cpp
        core/test_module_integration.cpp
        core/test_capi.cpp
        cli/test_cli_parser_server.cpp
        cli/test_cli_parser_client.cpp
)

add_executable(aqua_tests ${TEST_SOURCES})

target_link_libraries(aqua_tests PRIVATE
        aqua_core
        aqua_capi
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

# cli_parser_*.cpp 不编译进 aqua_core（只属于 CLI 可执行文件），
# cli 测试需要单独编译这两个源文件。
target_sources(aqua_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/cli/cli_parser_server.cpp
        ${CMAKE_SOURCE_DIR}/src/app/cli/cli_parser_client.cpp
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
