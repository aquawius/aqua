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
    try {
        // 设置信号处理
        auto& sig_handler = signal_handler::get_instance();
        sig_handler.setup();

        // 设置日志
        spdlog::set_level(spdlog::level::trace);
        spdlog::info("[main] {} version: {} platform: {}",
            aqua_client_BINARY_NAME, aqua_client_VERSION, aqua_client_PLATFORM_NAME);

        // 生成随机客户端端口
        std::random_device rd;
        std::default_random_engine gen(rd());
        std::uniform_int_distribution<uint16_t> dist(49152, 65535);
        const auto random_client_port = dist(gen);
        spdlog::info("[main] Using client port: {}", random_client_port);

        // 配置网络客户端
        network_client::client_config config {
            .server_address = "127.0.0.1",
            .server_port = 10120,
            .client_address = "127.0.0.1",
            .client_port = random_client_port
        };

        // 创建并启动客户端
        auto client = std::make_unique<network_client>(config);

        // 注册信号处理回调
        std::atomic<bool> running{true};
        sig_handler.register_callback([&running, &client]() {
            spdlog::warn("[main] Received shutdown signal");
            running = false;
            if (client) {
                spdlog::info("[main] Stopping network client...");
                client->stop_client();
            }
        });

        // 启动客户端
        if (!client->start_client()) {
            spdlog::error("[main] Failed to start network client");
            return EXIT_FAILURE;
        }

        spdlog::info("[main] Client started successfully");
        spdlog::info("[main] Running... Press Ctrl+C to stop");

        // 主循环
        const auto status_interval = std::chrono::seconds(5);
        auto last_status = std::chrono::steady_clock::now();

        while (running) {
            auto now = std::chrono::steady_clock::now();

            // 定期打印状态信息
            if (now - last_status >= status_interval) {
                if (client->is_running()) {
                    spdlog::info("[main] Status - Connected: {}, Bytes received: {}",
                        client->is_connected(),
                        client->get_total_bytes_received());
                }
                last_status = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // 清理资源
        spdlog::info("[main] Shutting down...");
        if (client) {
            client->stop_client();
            client.reset();
        }

        spdlog::info("[main] Application exited gracefully");
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        spdlog::critical("[main] Unhandled exception: {}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        spdlog::critical("[main] Unknown exception occurred");
        return EXIT_FAILURE;
    }
}
