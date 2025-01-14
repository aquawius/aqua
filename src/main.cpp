#include "config.h"

#include <iostream>
#include <format>
#include <print>

#include <uncomplete_functions.hpp>
#include <cxxopts.hpp>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include "cmdline_parser.h"
#include "signal_handler.h"
#include "linux/audio_manager_impl_linux.h"

int main(int argc, const char* argv[])
{
    try {

        aqua::cmdline_parser parser(argc, argv);
        auto result = parser.parse();

        if (result.help) {
            // std::cout << parser.get_help_string();
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

        audio_manager_impl audio_manager;

        if (!audio_manager.init()) {
            return 1;
        }

        if (!audio_manager.setup_stream()) {
            return 1;
        }

        // TODO: callback
        auto& handler = signal_handler::get_instance();
        handler.setup();
        handler.register_callback([&audio_manager]() {
            spdlog::debug("Triggered SIGNAL audio_manager stop callback...");
            audio_manager.stop_capture_request();
        });

        audio_manager.start_capture([](const std::vector<float>& data) {
            // 处理音频数据
            // spdlog::info("Received {} samples", data.size());
        });

        std::atomic<bool> running { true };
        handler.register_callback([&running]() {
            running = false;
        });

        spdlog::info("Running... Press Ctrl+C to stop");
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        spdlog::warn("Force return.");
        return 0;


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