//
// Created by aquawius on 25-1-23.
//

#include "rpc_test_client.h"

#include <iostream>
#include <thread>

int main()
{
    auto channel = grpc::CreateChannel("127.0.0.1:10120", grpc::InsecureChannelCredentials());
    TestRpcClient client(channel);

    std::string myUuid;
    // 向服务器发起 Connect 请求
    if (!client.Connect("127.0.0.1", 11111, myUuid)) {
        std::cerr << "Connect Fail" << std::endl;
        return 1;
    }

    // 每秒发送一次 keepalive，持续 30 秒
    constexpr int duration_in_seconds = 30;
    for (int i = 0; i < duration_in_seconds; ++i) {
        if (!client.KeepAlive(myUuid)) {
            std::cerr << "KeepAlive Fail at second: " << i << std::endl;
            // 根据需求决定是否要继续发送或直接跳出
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 断开连接
    if (!client.Disconnect(myUuid)) {
        std::cerr << "Disconnect Fail" << std::endl;
    }

    return 0;
}
