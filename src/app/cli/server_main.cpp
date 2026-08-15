#include "app/cli/cli_parser_server.h"
#include "core/logger/logger.h"
#include "core/public/config.h"
#include "core/server/server_runtime.h"

#include <atomic>
#include <csignal>
#include <iostream>

namespace {
// 信号处理只做原子置位（signal-safe），由 ServerRuntime::run(stop_when) 轮询感知。
std::atomic<bool> g_stop { false };

void signal_handler(int)
{
    g_stop = true;
}
} // namespace

int main(int argc, char** argv)
{
    auto parsed = aqua::parse_server_command_line(argc, argv);

    if (!parsed.success) {
        std::cerr << "Error: " << parsed.error_message << "\n";
        return 1;
    }
    if (parsed.show_help) {
        std::cout << parsed.help_message;
        return 0;
    }
    if (parsed.show_version) {
        std::cout << "aqua_server " << aqua::config::AQUA_VERSION << "\n";
        return 0;
    }

    aqua::set_log_level(parsed.log_level);

    // ---- CLI 参数 → 运行时配置（"0 = 用默认值"语义在此解析）----
    aqua::server::ServerConfig cfg;
    cfg.bind_ip = parsed.bind_ip;
    cfg.rpc_port = parsed.rpc_port;
    cfg.udp_port = parsed.udp_port;
    if (parsed.capture_buffer_size > 0) {
        cfg.runtime.capture_ringbuffer_size = parsed.capture_buffer_size;
    }

    // ---- 启动并运行（编排逻辑全部在 core 的 ServerRuntime 内）----
    aqua::server::ServerRuntime runtime;
    if (!runtime.start(cfg)) {
        std::cerr << "Error: " << runtime.last_error() << "\n";
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    runtime.run([&] { return g_stop.load(); });

    aqua::log_info("Server stopped.");
    return 0;
}
