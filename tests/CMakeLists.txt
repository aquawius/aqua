set(TEST_SOURCES
        test_log.cpp
        test_config.cpp
        test_session_manager.cpp
        test_cli_parser_server.cpp
        test_cli_parser_client.cpp
        test_audio_format.cpp
        test_audio_format_converter.cpp
        test_ringbuffer.cpp
        test_packet.cpp
        test_udp_transport.cpp
        test_nat_flow.cpp
        test_data_flow.cpp
        test_session_lifecycle.cpp
        test_jitter_buffer.cpp
        test_diagnostics.cpp
        test_end_to_end.cpp
        test_concurrency.cpp
        test_module_integration.cpp
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
)

target_sources(aqua_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/cli/cli_parser_server.cpp
        ${CMAKE_SOURCE_DIR}/src/app/cli/cli_parser_client.cpp
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
