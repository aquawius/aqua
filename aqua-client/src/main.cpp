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
        aqua_client::cmdline_parser parser(argc, argv);
        auto result = parser.parse();

        if (result.help) {
            fmt::print(fmt::runtime(aqua_client::cmdline_parser::get_help_string()));
            return EXIT_SUCCESS;
        }
        if (result.version) {
            fmt::print("{}\nversion: {}\nplatform: {}\n",
                aqua_client_BINARY_NAME, aqua_client_VERSION, aqua_client_PLATFORM_NAME);
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

        // 初始化音频管理器 (先创建音频管理器)
        auto audio_playback = audio_playback::create();
        if (!audio_playback || !audio_playback->init()) {
            return EXIT_FAILURE;
        }
        // 注意，这里没有调用audio_playback::setup_stream(), 因为需要当rpc获得的服务器数据回来才能知道需要初始化成什么流格式

        audio_playback->set_peak_callback(display_volume);

        // 创建并启动客户端
        auto client = std::make_unique<network_client>(audio_playback, config);

        // 网络模块异常处理回调
        client->set_shutdown_callback([&]() {
            spdlog::warn("[main] Server connection lost, triggering shutdown...");
            running = false;
        });

        // 启动客户端
        if (!client->start_client()) {
            spdlog::error("[main] Failed to start network client");
            return EXIT_FAILURE;
        }
        spdlog::info("[main] Network client started");

        // 设置信号处理
        auto& sig_handler = signal_handler::get_instance();
        sig_handler.setup();

        // 注册网络停止回调
        sig_handler.register_callback([&client]() {
            spdlog::debug("[main] Triggered SIGNAL network manager stop callback...");
            client->stop_client();
        });

        // 注册音频停止回调
        sig_handler.register_callback([&audio_playback]() {
            spdlog::debug("[main] Triggered SIGNAL audio_manager stop callback...");
            audio_playback->stop_playback();
        });

        // 注册main running状态修改
        sig_handler.register_callback([&running]() {
            spdlog::debug("[main] Triggered SIGNAL main running state change...");
            running = false;
        });

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