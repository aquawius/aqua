# tests/diagnostics/CMakeLists.txt
# diagnostics 模块测试：SnapshotLine（行组装）/ SnapshotView（块渲染）/ FieldBlock（k=v 拼接与 T/D/R）。

add_executable(aqua_diagnostics_tests
        snapshot_line_test.cpp
        field_block_test.cpp
        snapshot_view_test.cpp
)

target_link_libraries(aqua_diagnostics_tests PRIVATE
        aqua_core_base
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_diagnostics_tests)
