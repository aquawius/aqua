//
// Created by aquawius on 25-1-23.
//

#include "rpc_test_client.h"

#include <iostream>
#include <thread>

int main()
{
    // 使用随机数引擎生成可能可用的端口
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // TODO: testing, remove this in release.
    randomPort = 45678;

    // 使用生成的端口进行连接
    const auto channel = grpc::CreateChannel("127.0.0.1:10120", grpc::InsecureChannelCredentials());
    rpc_client client(channel);

    std::string myUuid;
    if (!client.connect("127.0.0.1", randomPort, myUuid)) {
        std::cerr << "Connect Fail: port=" << randomPort << std::endl;
        return 1;
    }


    // 每秒发送一次 keepalive，持续秒数
    constexpr int duration_in_seconds = 10;

    for (int i = 0; i < duration_in_seconds; ++i) {
        if (!client.keep_alive(myUuid)) {
            std::cerr << "KeepAlive Fail at second: " << i << std::endl;
            // 根据需求决定是否要继续发送或直接跳出
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 断开连接
    if (!client.disconnect(myUuid)) {
        std::cerr << "Disconnect Fail" << std::endl;
    }

    return 0;
}
