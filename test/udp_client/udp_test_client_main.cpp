#include "udp_test_client.h"

#include <chrono>
#include <random>
#include <thread>

#include <spdlog/spdlog.h>

int main()
{
    // 生成随机端口，范围可根据需要自行调整
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<int> dist(49152, 65535);
    unsigned short randomPort = static_cast<unsigned short>(dist(gen));

    // 测试时可固定端口
    randomPort = 45678;

    udp_client client("0.0.0.0", randomPort);
    client.start();

    constexpr int duration_in_seconds = 30;
    for (int i = 0; i < duration_in_seconds; ++i) {
        spdlog::info("[udp_test_client_main] Client running for: {}", (i + 1));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    client.stop();
    spdlog::info("[udp_test_client_main] UDP client stopped");

    return 0;
}
