#include "version.h"

#include <cxxopts.hpp>
#include <iostream>
#include <print>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "audio_manager.h"
#include "cmdline_parser.h"
#include "network_server.h"
#include "signal_handler.h"

void wait_n_sec(int n)
{
    for (int i = 1; i <= n; i++) {
        spdlog::info("[TEST] Waiting for [{}]/[{}] sec.", i, n);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
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
        aqua::cmdline_parser parser(argc, argv);
        auto result = parser.parse();

        if (result.help) {
            fmt::print(fmt::runtime(aqua::cmdline_parser::get_help_string()));
            return EXIT_SUCCESS;
        }

        if (result.version) {
            fmt::print("{}\nversion: {}\nplatform: {}\n",
                aqua_server_BINARY_NAME, aqua_server_VERSION, aqua_server_PLATFORM_NAME);
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

        // 初始化音频管理器 (先创建音频管理器)
        auto audio_manager = audio_manager::create();
        if (!audio_manager || !audio_manager->init()) {
            return EXIT_FAILURE;
        }

        if (!audio_manager->setup_stream(audio_manager->get_preferred_format())) {
            return EXIT_FAILURE;
        }
        spdlog::info("[main] Audio manager initialized");

        // 初始化network_server (传入音频管理器)
        std::string bind_address = result.bind_address.empty() ? network_server::get_default_address() : result.bind_address;

        std::unique_ptr<network_server> network = network_server::create(audio_manager, bind_address, result.port, result.port);
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

        if (!network->start_server()) {
            spdlog::error("[main] Failed to start network server");
            return EXIT_FAILURE;
        }
        spdlog::info("[main] Network manager started");

        // 设置信号处理
        auto& signal_handler = signal_handler::get_instance();
        signal_handler.setup();

        // 注册网络停止回调
        signal_handler.register_callback([&network]() {
            spdlog::debug("[main] Triggered SIGNAL network manager stop callback...");
            network->stop_server();
        });

        // 注册音频停止回调
        signal_handler.register_callback([&audio_manager]() {
            spdlog::debug("[main] Triggered SIGNAL audio_manager stop callback...");
            audio_manager->stop_capture();
        });

        // 注册main running状态修改
        signal_handler.register_callback([&running]() {
            spdlog::debug("[main] Triggered SIGNAL main running state change...");
            running = false;
        });

        // 启动音频捕获，并将数据发送到网络
        audio_manager->start_capture([&network](const std::span<const std::byte> data) {
            if (data.empty()) {
                return;
            }

            network->push_audio_data(data);
        });

        // console peak display
        audio_manager->set_peak_callback(display_volume);

        spdlog::info("[main] Running... Press Ctrl+C to stop");

        // TEST for change format.
        wait_n_sec(10);
        audio_manager->reconfigure_stream(audio_common::AudioFormat(audio_common::AudioEncoding::PCM_S32LE, 2, 48000));
        wait_n_sec(3);
        audio_manager->reconfigure_stream(audio_manager->get_preferred_format());

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