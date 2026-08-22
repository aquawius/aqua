# tests/core/net/CMakeLists.txt
#
# net 模块测试：address_utils / UDP transport / gRPC（client、server、格式转换）。
# 由 tests/CMakeLists.txt 通过 add_subdirectory(core/net) 引入。
#
# 直接链接 aqua_core：asio / gRPC / protobuf / 生成的 pb 头文件均随其传递，
# 因此这里不需要额外的 include 目录或第三方库链接。
# （若需要脱离主工程单独构建这些测试，可参考 git 历史中带 AQUA_NET_BUILD_SOURCES
#   独立构建模式的版本，但 gRPC 测试还需生成头目录与 grpc 库，维护成本较高。）

find_package(Threads REQUIRED) # std::thread（Linux 需 -pthread）

set(AQUA_NET_TEST_SOURCES
    address/test_address_utils.cpp
    address/test_address_edge_cases.cpp
    transport/test_udp_loopback.cpp
    transport/test_udp_edge_cases.cpp
    grpc/test_grpc_audio_format_converter.cpp
    grpc/test_grpc_client.cpp
    grpc/test_grpc_server.cpp
    grpc/test_grpc_edge_cases.cpp
)

add_executable(aqua_net_tests ${AQUA_NET_TEST_SOURCES})

target_link_libraries(aqua_net_tests PRIVATE
    aqua_core
    GTest::gtest_main
    Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(aqua_net_tests)
