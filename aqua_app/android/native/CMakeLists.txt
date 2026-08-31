# aqua_app/android/native：Android（及 host 冒烟）共用的 C API 边界。
#
# 产出共享库 aqua_capi（OUTPUT_NAME=aqua → libaqua.so / aqua.dll）：
#   - Android 交叉编译（android-arm64-* preset）：链 aqua_client_core + aaudio，
#     供 Gradle jniLibs 打包；
#   - Windows host：同一份代码可编成 DLL 并跑 aqua_capi_test 冒烟（WASAPI 路径），
#     保证 C API 与监督线程不依赖真机即可验证。
#
# 由 aqua_app/CMakeLists.txt 以 AQUA_BUILD_ANDROID=ON 引入。

add_library(aqua_capi SHARED
        aqua_capi.cpp
)
target_link_libraries(aqua_capi PRIVATE
        aqua_client_core
)
# 产物名统一为 aqua（libaqua.so / aqua.dll），与旧 Android 工程的
# System.loadLibrary("aqua") 约定一致。
set_target_properties(aqua_capi PROPERTIES
        OUTPUT_NAME aqua
)

if (WIN32)
    # C 接口无 __declspec(dllexport) 注解，按符号全导出（仅 C 函数，安全）。
    set_target_properties(aqua_capi PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

if (ANDROID)
    # AAudio playback backend + logcat（spdlog android sink 需要 liblog）。
    target_link_libraries(aqua_capi PRIVATE aaudio log)
endif ()

# ---- host 冒烟测试（Windows/Linux/macOS）----
# 验证 C API 生命周期、枚举镜像与监督线程装配；不需要真 server
# （start 指向无人监听的端口，走 START_FAILED 路径）。
if (AQUA_BUILD_TEST AND NOT ANDROID)
    add_executable(aqua_capi_test
            aqua_capi_test.cpp
    )
    target_link_libraries(aqua_capi_test PRIVATE
            aqua_capi
            GTest::gtest_main
    )
    add_test(NAME aqua_capi_test COMMAND aqua_capi_test)
endif ()
