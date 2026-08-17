#include "app/cli/cli_parser_client.h"
#include "app/cli/cli_version.h"
#include "core/client/client_runtime.h"
#include "core/logger/logger.h"

#include <atomic>
#include <csignal>
#include <iostream>

namespace {
// 信号处理只做原子置位（signal-safe），由 ClientRuntime::run(stop_when) 轮询感知。
std::atomic<bool> g_stop { false };

void signal_handler(int)
{
    g_stop = true;
}
} // namespace

int main(int argc, char** argv)
{
    auto parsed = aqua::parse_client_command_line(argc, argv);

    if (!parsed.success) {
        std::cerr << "Error: " << parsed.error_message << "\n";
        return 1;
    }
    if (parsed.show_help) {
        std::cout << parsed.help_message;
        return 0;
    }
    if (parsed.show_version) {
        std::cout << "aqua_client " << AQUA_CLIENT_CLI_VERSION << "\n";
        return 0;
    }

    aqua::set_log_level(parsed.log_level);

    // ---- CLI 参数 → 运行时配置（"0 = 用默认值"语义在此解析）----
    aqua::client::ClientConfig cfg;
    cfg.server_ip = parsed.server_ip;
    cfg.server_rpc_port = parsed.server_rpc_port;
    cfg.auto_reconnect = parsed.auto_reconnect;
    if (parsed.jitter_buffer_ms > 0) {
        cfg.runtime.jitter_buffer_ms = parsed.jitter_buffer_ms;
    }
    if (parsed.jitter_detect_window_packets > 0) {
        cfg.runtime.jitter_detect_window_packets = parsed.jitter_detect_window_packets;
    }
    if (parsed.playback_buffer_size > 0) {
        cfg.runtime.playback_ringbuffer_size = parsed.playback_buffer_size;
    }

    // ---- 启动并运行（编排逻辑全部在 core 的 ClientRuntime 内）----
    aqua::client::ClientRuntime runtime;
    if (!runtime.start(cfg)) {
        std::cerr << "Error: client already running\n";
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    runtime.run([&] { return g_stop.load(); });

    const bool failed = (runtime.state() == aqua::client::ClientState::Failed);
    if (failed) {
        std::cerr << "Error: " << runtime.last_error() << "\n";
    }

    aqua::log_info("Client stopped.");
    return failed ? 1 : 0;
}
