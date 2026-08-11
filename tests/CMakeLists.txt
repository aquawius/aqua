set(TEST_SOURCES
        test_log.cpp
)

add_executable(aqua_tests ${TEST_SOURCES})

target_link_libraries(aqua_tests PRIVATE
        aqua_core
        aqua_proto
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
