set(TEST_SOURCES
        test_log.cpp
        test_session_manager.cpp
        test_cli_parser_server.cpp
        test_cli_parser_client.cpp
        test_audio_format.cpp
        test_ringbuffer.cpp
        test_packet.cpp
        test_udp_transport.cpp
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
        ${CMAKE_SOURCE_DIR}/src/main/cli_parser_server.cpp
        ${CMAKE_SOURCE_DIR}/src/main/cli_parser_client.cpp
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
