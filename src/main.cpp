#include "config.h"

#include <cxxopts.hpp>
#include <iostream>
#include <print>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "audio_manager.h"
#include "cmdline_parser.h"
#include "network_server.h"
#include "signal_handler.h"

void wait_3_sec()
{
    spdlog::info("[TEST] Waiting for 3 sec.");
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

int main(int argc, const char* argv[])
{
    try {
        aqua::cmdline_parser parser(argc, argv);
        auto result = parser.parse();

        if (result.help) {
            fmt::print(fmt::runtime(aqua::cmdline_parser::get_help_string()));
            return EXIT_SUCCESS;
        }

        if (result.version) {
            fmt::print("{}\nversion: {}\nplatform: {}\n",
                aqua_core_BINARY_NAME, aqua_core_VERSION, aqua_core_PLATFORM_NAME);
            return EXIT_SUCCESS;
        }

        // 设置日志级别
        spdlog::set_level(result.log_level);
        if (result.log_level <= spdlog::level::debug) {
            spdlog::debug("[main] Debug mode enabled");
        }
        if (result.log_level <= spdlog::level::trace) {
            spdlog::trace("[main] Trace mode enabled");
        }

        std::atomic<bool> running { true };

        // 初始化network_server
        std::string bind_address = result.bind_address.empty() ? network_server::get_default_address() : result.bind_address;

        std::unique_ptr<network_server> network = network_server::create(bind_address, result.port, result.port);
        if (!network) {
            spdlog::error("[main] Failed to initialize network manager");
            return EXIT_FAILURE;
        }
        spdlog::info("[main] Network manager initialized with address {}:{}", bind_address, result.port);

        // 异常回调
        network->set_shutdown_callback([&running]() {
            spdlog::warn("[main] Network server shutdown, triggering exit...");
            running = false;
        });

        network->start_server();
        spdlog::info("[main] Network manager started");

        // 初始化音频管理器
        auto audio_manager = audio_manager::create();
        if (!audio_manager || !audio_manager->init()) {
            return EXIT_FAILURE;
        }

        if (!audio_manager->setup_stream()) {
            return EXIT_FAILURE;
        }
        spdlog::info("[main] Audio manager initialized");

        // 设置信号处理
        auto& signal_handler = signal_handler::get_instance();
        signal_handler.setup();

        // 注册音频停止回调
        signal_handler.register_callback([&audio_manager]() {
            spdlog::debug("[main] Triggered SIGNAL audio_manager stop callback...");
            audio_manager->stop_capture();
        });

        // 注册网络停止回调
        signal_handler.register_callback([&network]() {
            spdlog::debug("[main] Triggered SIGNAL network manager stop callback...");
            network->stop_server();
        });

        // 启动音频捕获，并将数据发送到网络
        audio_manager->start_capture([&network](const std::span<const float> data) {
            if (data.empty()) {
                return;
            }

            // 发送音频数据
            network->push_audio_data(data);
        });

        // 主循环
        signal_handler.register_callback([&running]() {
            running = false;
        });

        spdlog::info("[main] Running... Press Ctrl+C to stop");
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        spdlog::info("[main] Shutting down...");

        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        spdlog::critical("[main] Unhandled exception: {}", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        spdlog::critical("[main] Unknown exception occurred");
        return EXIT_FAILURE;
    }
}