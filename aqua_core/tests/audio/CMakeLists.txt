# tests/audio/CMakeLists.txt
# audio 模块测试：领域模型（audio_format / audio_frame）+ WASAPI 设备后端。
# 与 net / logger 测试一致，统一使用 Google Test（GTest::gtest_main + gtest_discover_tests）。

add_executable(aqua_audio_tests
        audio_format_test.cpp
        audio_frame_queue_test.cpp
)

target_link_libraries(aqua_audio_tests PRIVATE
        aqua_core_base
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_audio_tests)

# JitterBuffer 属于回放侧（client core），测试链接 aqua_client_core。
add_executable(aqua_jitter_buffer_tests
        jitter_buffer_test.cpp
        jitter_buffer_boundary_test.cpp
)

target_link_libraries(aqua_jitter_buffer_tests PRIVATE
        aqua_client_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_jitter_buffer_tests)

# 打包器属于采集侧（server core），测试链接 aqua_server_core。
add_executable(aqua_audio_packetizer_tests
        packetizer/audio_packetizer_test.cpp
        packetizer/audio_packetizer_boundary_test.cpp
)

target_link_libraries(aqua_audio_packetizer_tests PRIVATE
        aqua_server_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(aqua_audio_packetizer_tests)

# 解包（wire decode → AudioFrame）已并入 UDP client 数据面协议层
#（net/udp/udp_client），测试见 tests/net/udp/。

# WASAPI 后端测试依赖 Windows 音频设备，仅 Windows 构建。
if (WIN32)
    add_executable(aqua_wasapi_device_manager_tests
            wasapi/wasapi_device_manager_test.cpp
    )
    target_link_libraries(aqua_wasapi_device_manager_tests PRIVATE
            aqua_server_core
            GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(aqua_wasapi_device_manager_tests)
endif ()

if (WIN32)
    add_executable(aqua_wasapi_capture_tests
            wasapi/wasapi_audio_capture_test.cpp
    )
    target_link_libraries(aqua_wasapi_capture_tests PRIVATE
            aqua_server_core
            GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(aqua_wasapi_capture_tests)
endif ()

if (WIN32)
    add_executable(aqua_wasapi_playback_tests
            wasapi/wasapi_audio_playback_test.cpp
    )
    target_link_libraries(aqua_wasapi_playback_tests PRIVATE
            aqua_client_core
            GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(aqua_wasapi_playback_tests)
endif ()
