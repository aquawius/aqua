#ifndef AQUA_CLI_PARSER_CLIENT_H
#define AQUA_CLI_PARSER_CLIENT_H

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
    uint16_t server_udp_port = 50000;
};

ClientCliResult parse_client_command_line(int argc, const char* const* argv);
ClientCliResult parse_client_command_line(const std::vector<std::string>& args);

} // namespace aqua

#endif // AQUA_CLI_PARSER_CLIENT_H
