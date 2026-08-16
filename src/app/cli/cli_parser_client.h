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
    // JitterBuffer 总容量（毫秒）。唯一 JB 参数，floor/ceiling/capacity
    // 由 core 内部按固定比例推导（0 = 用 config.h 默认值 60ms）
    uint32_t jitter_buffer_ms = 0;
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
