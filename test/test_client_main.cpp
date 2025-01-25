//
// Created by aquawius on 25-1-25.
//

#include <iostream>
#include <thread>
#include <random>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "udp_client/udp_test_client.h"
#include "rpc_client/rpc_test_client.h"

// 若不需要固定端口测试，可删掉固定赋值
constexpr unsigned short kTestFixedPort = 45678;

int main()
{
    // 1. 生成随机端口（可根据需求修改范围）
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // 2. 测试时可固定为某个端口
    randomPort = kTestFixedPort;

    // 3. 先启动 RPC 客户端
    //    目标服务器在 "127.0.0.1:10120"（示例）
    auto channel = grpc::CreateChannel("127.0.0.1:10120",
                                       grpc::InsecureChannelCredentials());
    rpc_client rpcTestClient(channel);

    std::string myUuid;
    if (!rpcTestClient.connect("127.0.0.1", randomPort, myUuid)) {
        std::cerr << "[rpc_udp_combo] Connect RPC Fail, port=" << randomPort << std::endl;
        return 1;
    } else {
        std::cout << "[rpc_udp_combo] RPC Connected @ Port " << randomPort
                  << ", UUID = " << myUuid << std::endl;
    }

    // 4. 启动 UDP 客户端，使用相同端口
    udp_client udpTestClient("0.0.0.0", randomPort);
    udpTestClient.start();
    std::cout << "[rpc_udp_combo] UDP Client started @ Port " << randomPort << std::endl;

    // 5. 循环发送 keepalive 并观察 UDP
    constexpr int duration_in_seconds = 10;
    for (int i = 0; i < duration_in_seconds; ++i) {
        // 每次发送 keep_alive()
        if (!rpcTestClient.keep_alive(myUuid)) {
            std::cerr << "[rpc_udp_combo] KeepAlive Fail @ second: " << i << std::endl;
            // 根据需求决定是否直接退出或继续
        }

        // 等待 1 秒
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 6. 通知服务器断开 RPC 连接
    if (!rpcTestClient.disconnect(myUuid)) {
        std::cerr << "[rpc_udp_combo] RPC Disconnect Fail" << std::endl;
    } else {
        std::cout << "[rpc_udp_combo] RPC Disconnect Success" << std::endl;
    }

    // 7. 停止 UDP Client
    udpTestClient.stop();
    std::cout << "[rpc_udp_combo] UDP Client stopped" << std::endl;

    return 0;
}
