#ifndef AQUA_CLI_PARSER_SERVER_H
#define AQUA_CLI_PARSER_SERVER_H

#include "core/logger/logger.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aqua {

struct ServerCliResult {
    bool success = false;
    bool show_help = false;
    bool show_version = false;
    std::string help_message;
    std::string error_message;
    std::string bind_ip = "0.0.0.0";
    uint16_t rpc_port = 50051;
    uint16_t udp_port = 50000;
    // 日志等级。默认用编译期 default_log_level()；--log-level 覆盖。
    LogLevel log_level = default_log_level();
};

ServerCliResult parse_server_command_line(int argc, const char* const* argv);
ServerCliResult parse_server_command_line(const std::vector<std::string>& args);

} // namespace aqua

#endif // AQUA_CLI_PARSER_SERVER_H
