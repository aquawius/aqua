#ifndef AQUA_CLI_PARSER_CLIENT_H
#define AQUA_CLI_PARSER_CLIENT_H

#include "core/logger/logger.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aqua {

struct ClientCliResult {
    bool success = false;
    bool show_help = false;
    bool show_version = false;
    std::string help_message;
    std::string error_message;
    std::string server_ip = "127.0.0.1";
    uint16_t server_rpc_port = 50051;
    // JitterBuffer 默认目标延迟（0 = 用 config.h 默认值）
    uint32_t jitter_latency_ms = 0;
    // JitterBuffer 自适应 target 上限（0 = 不启用，自适应上限 = capacity/2）
    uint32_t jitter_max_latency_ms = 0;
    // JitterBuffer 自适应评估窗口包数（0 = 用 config.h 默认值）
    uint32_t jitter_adapt_window_packets = 0;
    // JitterBuffer 漂移检测 late 阈值（0 = 用 config.h 默认值）
    uint32_t drift_late_threshold = 0;
    // 播放 RingBuffer 大小（字节，0 = 用 config.h 默认值）
    std::size_t playback_buffer_size = 0;
    // 断线自动重连（指数退避），默认关闭
    bool auto_reconnect = false;
    // 日志等级。默认用编译期 default_log_level()；--log-level 覆盖。
    LogLevel log_level = default_log_level();
};

ClientCliResult parse_client_command_line(int argc, const char* const* argv);
ClientCliResult parse_client_command_line(const std::vector<std::string>& args);

} // namespace aqua

#endif // AQUA_CLI_PARSER_CLIENT_H
