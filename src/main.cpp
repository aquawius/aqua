// main.cpp
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "network_client.h"
#include "signal_handler.h"

void wait_3_sec()
{
    spdlog::info("[TEST] Waiting for 3 sec.");
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

int main(int argc, const char* argv[])
{
    auto& sig_handler = signal_handler::get_instance();
    sig_handler.setup();

    spdlog::set_level(spdlog::level::trace);
    spdlog::info("{}\tversion: {}\tplatform: {}",
        aqua_client_BINARY_NAME, aqua_client_VERSION, aqua_client_PLATFORM_NAME);

    // 生成随机客户端端口
    std::random_device rd;
    std::default_random_engine gen(rd());
    std::uniform_int_distribution<unsigned short> dist(49152, 65535);
    const auto client_port = dist(gen);
    spdlog::info("Using client port: {}", client_port);

    // 配置网络客户端
    network_client::client_config config {
        .server_address = "127.0.0.1",
        .server_port = 10120,
        .client_address = "127.0.0.1",
        .client_port = client_port,
    };

    network_client client(config);

    // 注册网络停止回调
    sig_handler.register_callback([&client]() {
        spdlog::warn("[main] Stopping network client...");
        client.stop();
    });

    // 启动客户端
    if (!client.start()) {
        spdlog::error("Failed to start network client");
        return 1;
    }


    // 主循环
    std::atomic<bool> running { true };
    sig_handler.register_callback([&running]() {
        running = false;
    });

    spdlog::info("[main] Running... Press Ctrl+C to stop");
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("[main] Shutting down...");

    return EXIT_SUCCESS;

    client.stop();
    spdlog::info("Application exited gracefully");
    return 0;
}