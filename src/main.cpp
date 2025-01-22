#include <iostream>
#include <format>
#include <print>

#include <cxxopts.hpp>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "cmdline_parser.h"
#include "network_server.h"
#include "signal_handler.h"
#include "linux/audio_manager_impl_linux.h"

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

        if (result.verbose) {
            spdlog::set_level(spdlog::level::trace);
            spdlog::trace("Verbose mode.");
        }

        std::unique_ptr<network_server> network = network_server::create(network_server::get_default_address());
        if (!network) {
            spdlog::error("Failed to initialize network manager");
            return EXIT_FAILURE;
        }

        for (auto address_list : network->get_address_list()) {
            spdlog::trace("address list: {}", address_list);
        }

        spdlog::trace("default address: {}", network->get_default_address());

        // 初始化音频管理器
        audio_manager_impl audio_manager;
        if (!audio_manager.init()) {
            return EXIT_FAILURE;
        }
        if (!audio_manager.setup_stream()) {
            return EXIT_FAILURE;
        }

        // 设置信号处理
        auto& signal_handler = signal_handler::get_instance();
        signal_handler.setup();

        // 注册音频停止回调
        signal_handler.register_callback([&audio_manager]() {
            spdlog::debug("Triggered SIGNAL audio_manager stop callback...");
            audio_manager.stop_capture_request();
        });

        // 注册网络停止回调
        signal_handler.register_callback([&network]() {
            spdlog::debug("Triggered SIGNAL network manager stop callback...");
            network->stop_server();
        });

        // 启动音频捕获，并将数据发送到网络
        audio_manager.start_capture([&network](const std::vector<float>& data) {
            network->push_audio_data(data);

            // 可选：添加调试日志
            if (spdlog::get_level() <= spdlog::level::debug) {
                static uint64_t packet_count = 0;
                if (++packet_count % 100 == 0) { // 每100个包打印一次日志
                    spdlog::debug("Sent audio packet: {} samples", data.size());
                }
            }
        });

        // 主循环
        std::atomic<bool> running { true };
        signal_handler.register_callback([&running]() {
            running = false;
        });

        spdlog::info("Running... Press Ctrl+C to stop");
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        spdlog::info("Shutting down...");

        return EXIT_SUCCESS;

        // ########################################################################
        // return -1;
        // TODO:

        if (result.list_encoding) {
            std::vector<std::pair<std::string, std::string>> array = {
                { "default", "Default encoding" },
                { "f32", "32 bit floating-point PCM" },
                { "s8", "8 bit integer PCM" },
                { "s16", "16 bit integer PCM" },
                { "s24", "24 bit integer PCM" },
                { "s32", "32 bit integer PCM" },
            };
            for (auto&& e : array) {
                // fmt::println("\t{}\t\t{}", e.first, e.second);
            }
            return EXIT_SUCCESS;
        }

        if (!result.bind_address.empty()) {
            // 处理bind地址和启动服务器的逻辑
            size_t pos = result.bind_address.find(':');
            std::string host = result.bind_address.substr(0, pos);
            uint16_t port = (pos == std::string::npos) ? 65530 : static_cast<uint16_t>(std::stoi(result.bind_address.substr(pos + 1)));

            // TODO: audio manager.
            spdlog::info("Bind address {}:{}", host, port);

            return EXIT_SUCCESS;
        }

        // 没有参数时显示帮助信息
        fmt::print(fmt::runtime(parser.get_help_string()));
        return EXIT_FAILURE;

    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return EXIT_FAILURE;
    }
}