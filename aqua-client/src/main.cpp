#include "cmdline_parser.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "version.h"
#include "network_client.h"
#include "signal_handler.h"

#include <random>

void wait_3_sec()
{
    spdlog::info("[TEST] Waiting for 3 sec.");
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

void display_volume(const float peak_val)
{
    if (spdlog::get_level() > spdlog::level::debug) {
        return;
    }

    constexpr size_t METER_WIDTH = 40;
    static std::array<char, METER_WIDTH + 1> meter_buffer;
    meter_buffer.fill('-');

    // 计算峰值电平并更新音量条
    const int peak_level = std::clamp(static_cast<int>(peak_val * METER_WIDTH), 0,
        static_cast<int>(METER_WIDTH));

    if (peak_level > 0) {
        std::fill_n(meter_buffer.begin(), peak_level, '#');
    }

    meter_buffer[METER_WIDTH] = '\0';
    spdlog::debug("[{}] {:.3f}", meter_buffer.data(), peak_val);
}

int main(int argc, const char* argv[])
{
    try {

        // 解析命令行参数
        aqua_client::cmdline_parser parser(argc, argv);
        auto result = parser.parse();

        if (result.help) {
            std::cout << aqua_client::cmdline_parser::get_help_string();
            return EXIT_SUCCESS;
        }
        if (result.version) {
            spdlog::info("{} version: {}", aqua_client_BINARY_NAME, aqua_client_VERSION);
            return EXIT_SUCCESS;
        };

        // 设置日志级别
        spdlog::set_level(result.log_level);
        if (result.log_level <= spdlog::level::debug) {
            spdlog::debug("[main] Debug mode enabled");
        }
        if (result.log_level <= spdlog::level::trace) {
            spdlog::trace("[main] Trace mode enabled");
        }

        // 生成随机客户端端口（如果未指定）
        uint16_t client_udp_port = result.client_udp_port;
        if (client_udp_port == 0) {
            std::random_device rd;
            std::default_random_engine gen(rd());
            std::uniform_int_distribution<uint16_t> dist(49152, 65535);
            client_udp_port = dist(gen);
        }

        // 构建配置
        network_client::client_config config {
            .server_address = result.server_address,
            .server_rpc_port = result.server_rpc_port,
            .client_address = result.client_address,
            .client_udp_port = client_udp_port
        };

        // 设置信号处理
        auto& sig_handler = signal_handler::get_instance();
        sig_handler.setup();

        // 创建并启动客户端
        auto client = std::make_unique<network_client>(config);

        // 注册信号处理回调
        std::atomic<bool> running { true };
        sig_handler.register_callback([&running, &client]() {
            spdlog::warn("[main] Received shutdown signal");
            running = false;
            if (client) {
                spdlog::info("[main] Stopping network client...");
                client->stop_client();
            }
        });

        // 异常处理回调
        client->set_shutdown_callback([&running]() {
            spdlog::warn("[main] Server connection lost, triggering shutdown...");
            running = false;
        });

        // 启动客户端
        if (!client->start_client()) {
            spdlog::error("[main] Failed to start network client");
            return EXIT_FAILURE;
        }

        client->set_audio_peak_callback(display_volume);

        spdlog::info("[main] Client started successfully");
        spdlog::info("[main] Running... Press Ctrl+C to stop");

        while (running) {
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
        spdlog::critical("[main] Exception: {}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        spdlog::critical("[main] Unknown exception occurred");
        return EXIT_FAILURE;
    }
}