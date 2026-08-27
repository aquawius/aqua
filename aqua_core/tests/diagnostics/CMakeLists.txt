# tests/diagnostics/CMakeLists.txt
# diagnostics 模块测试（Diagnostics 指标源注册/打印）。

add_executable(aqua_diagnostics_tests
        diagnostics_test.cpp
)

target_link_libraries(aqua_diagnostics_tests PRIVATE
        aqua_core_base
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_diagnostics_tests)
