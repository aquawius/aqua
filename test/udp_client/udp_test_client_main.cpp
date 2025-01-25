#include "udp_test_client.h"

#include <iostream>
#include <thread>
#include <random>
#include <chrono>

int main()
{
    // 生成随机端口，范围可根据需要自行调整
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // TODO: testing, remove this in release.
    randomPort = 45678;

    // 创建并启动 UDP 客户端
    udp_client client("0.0.0.0", randomPort);
    client.start();

    // 运行时长0 秒）
    constexpr int duration_in_seconds = 10;
    for (int i = 0; i < duration_in_seconds; ++i) {
        std::cout << "Client running for: " << (i + 1) << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 关闭客户端
    client.stop();
    std::cout << "UDP client stoped" << std::endl;

    return 0;
}