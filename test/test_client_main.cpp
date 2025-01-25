//
// Created by aquawius on 25-1-25.
//

#include "rpc_client/rpc_test_client.h"
#include "udp_client/udp_test_client.h"

#include <chrono>
#include <random>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

constexpr unsigned short kTestFixedPort = 45678;

int main()
{
    // 随机生成端口
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // 测试时可固定端口
    // randomPort = kTestFixedPort;

    // 创建 gRPC 通道并初始化 RPC 客户端
    auto channel = grpc::CreateChannel("127.0.0.1:10120",
        grpc::InsecureChannelCredentials());
    rpc_client rpcTestClient(channel);

    std::string myUuid;
    if (!rpcTestClient.connect("127.0.0.1", randomPort, myUuid)) {
        spdlog::error("[test_client] Connect RPC Fail, port={}", randomPort);
        return 1;
    } else {
        spdlog::info("[test_client] RPC Connected @ Port {}, UUID = {}", randomPort, myUuid);
    }

    // 启动 UDP 客户端
    udp_client udpTestClient("0.0.0.0", randomPort);
    udpTestClient.start();
    spdlog::info("[test_client] UDP Client started @ Port {}", randomPort);

    // 连续发送 keepalive 并等待
    constexpr int duration_in_seconds = 600;
    for (int i = 0; i < duration_in_seconds; ++i) {
        if (!rpcTestClient.keep_alive(myUuid)) {
            spdlog::error("[test_client] KeepAlive Fail @ second {}", i);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 断开 RPC 连接
    if (!rpcTestClient.disconnect(myUuid)) {
        spdlog::error("[test_client] RPC Disconnect Fail");
    } else {
        spdlog::info("[test_client] RPC Disconnect Success");
    }

    // 停止 UDP 客户端
    udpTestClient.stop();
    spdlog::info("[test_client] UDP Client stopped");

    return 0;
}
