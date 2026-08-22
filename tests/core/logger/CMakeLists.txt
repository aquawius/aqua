# tests/core/logger/CMakeLists.txt
# logger 模块测试。由 tests/CMakeLists.txt 引入。

add_executable(aqua_tests
    test_core_logger.cpp
)

target_link_libraries(aqua_tests PRIVATE
    aqua_core       # logger 实现 + PUBLIC include（src），无需额外链接
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_tests)
