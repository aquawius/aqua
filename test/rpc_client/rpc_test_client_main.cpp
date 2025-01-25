//
// Created by aquawius on 25-1-23.
//

#include "rpc_test_client.h"

#include <chrono>
#include <random>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

int main()
{
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // 测试时可固定端口
    randomPort = 45678;

    auto channel = grpc::CreateChannel("127.0.0.1:10120",
        grpc::InsecureChannelCredentials());
    rpc_client client(channel);

    std::string myUuid;
    if (!client.connect("127.0.0.1", randomPort, myUuid)) {
        spdlog::error("[rpc_test_client_main] Connect Fail: port = {}", randomPort);
        return 1;
    }

    constexpr int duration_in_seconds = 10;
    for (int i = 0; i < duration_in_seconds; ++i) {
        if (!client.keep_alive(myUuid)) {
            spdlog::error("[rpc_test_client_main] KeepAlive Fail at second {}", i);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!client.disconnect(myUuid)) {
        spdlog::error("[rpc_test_client_main] Disconnect Fail");
    }

    return 0;
}
