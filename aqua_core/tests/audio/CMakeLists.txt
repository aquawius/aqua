# tests/audio/CMakeLists.txt
# audio 模块测试：领域模型（audio_format / audio_frame）+ WASAPI 设备后端。
# 与 net / logger 测试一致，统一使用 Google Test（GTest::gtest_main + gtest_discover_tests）。

add_executable(aqua_audio_tests
        audio_format_test.cpp
)

target_link_libraries(aqua_audio_tests PRIVATE
        aqua_core_base
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_audio_tests)

# WASAPI 后端测试依赖 Windows 音频设备，仅 Windows 构建。
if (WIN32)
    add_executable(aqua_wasapi_device_tests
            wasapi/wasapi_device_manager_test.cpp
    )
    target_link_libraries(aqua_wasapi_device_tests PRIVATE
            aqua_core_base
            GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(aqua_wasapi_device_tests)
endif ()
