#include "cli_parser_server.h"
#include "core/logger/logger.h"

#include <iostream>

constexpr char VERSION[] = "0.0.1";

int main(int argc, char** argv) {
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
        std::cout << "aqua_server " << VERSION << "\n";
        return 0;
    }

    aqua::log_info_fmt("Starting Aqua server on {} gRPC={}, UDP={}",
                       parsed.bind_ip, parsed.rpc_port, parsed.udp_port);

    // TODO: create gRPC server and UDP transport

    return 0;
}
