# tests/net/CMakeLists.txt
#
# net 模块测试：address_utils / UDP transport / gRPC（client、server、格式转换）。
# 由 aqua_core/tests/CMakeLists.txt 通过 add_subdirectory(net) 引入。
#
# 测试同时使用服务端与客户端（udp 环回、gRPC 往返），因此同时链接
# aqua_server_core 与 aqua_client_core；公共符号经两者传递的 aqua_core_base 提供。
# asio / gRPC / protobuf / 生成的 pb 头文件均随其传递，无需额外 include 目录。

find_package(Threads REQUIRED) # std::thread（Linux 需 -pthread）

set(AQUA_NET_TEST_SOURCES
        address/address_utils_test.cpp
        address/address_edge_cases_test.cpp
        udp/udp_loopback_test.cpp
        udp/udp_edge_cases_test.cpp
        grpc/grpc_audio_format_converter_test.cpp
        grpc/grpc_client_test.cpp
        grpc/grpc_server_test.cpp
        grpc/grpc_edge_cases_test.cpp
)

add_executable(aqua_net_tests ${AQUA_NET_TEST_SOURCES})

target_link_libraries(aqua_net_tests PRIVATE
        aqua_server_core
        aqua_client_core
        GTest::gtest_main
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(aqua_net_tests)
