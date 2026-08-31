# c_api 模块测试：C API host 冒烟（生命周期 / 枚举镜像 / null handle 防护）。
# 不依赖真 server / 真音频设备；仅 host 平台构建（Android preset AQUA_BUILD_TEST=OFF）。
if (NOT AQUA_BUILD_C_API)
    message(WARNING "tests/c_api: AQUA_BUILD_C_API is OFF; c_api tests skipped")
    return()
endif ()

add_executable(aqua_capi_test
        aqua_capi_test.cpp
)
target_link_libraries(aqua_capi_test PRIVATE
        aqua_capi
        GTest::gtest_main
)
target_include_directories(aqua_capi_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(aqua_capi_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 测试 exe 与 aqua_capi 同输出目录（<build>/bin），依赖 DLL 由 vcpkg
# applocal 就地拷贝，无需额外拷贝步骤。

include(GoogleTest)
gtest_discover_tests(aqua_capi_test)
