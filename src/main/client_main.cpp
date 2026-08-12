#include "cli_parser_client.h"
#include "core/logger/logger.h"

#include <iostream>

constexpr char VERSION[] = "0.0.1";

int main(int argc, char** argv) {
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
        std::cout << "aqua_client " << VERSION << "\n";
        return 0;
    }

    aqua::log_info_fmt("Starting Aqua client connecting to {}:{}",
                       parsed.server_ip, parsed.server_rpc_port);

    // TODO: create gRPC client and UDP transport

    return 0;
}
