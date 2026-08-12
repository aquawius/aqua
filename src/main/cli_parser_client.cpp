#include "cli_parser_client.h"

#include <cxxopts.hpp>
#include <sstream>

namespace aqua {

namespace {

std::optional<uint16_t> parse_port(const std::string& value, const std::string& name,
                                   std::string& error) {
    try {
        int port = std::stoi(value);
        if (port <= 0 || port > 65535) {
            error = name + " must be in range 1..65535";
            return std::nullopt;
        }
        return static_cast<uint16_t>(port);
    } catch (const std::exception&) {
        error = name + " must be a valid port number";
        return std::nullopt;
    }
}

} // namespace

ClientCliResult parse_client_command_line(int argc, const char* const* argv) {
    cxxopts::Options options("aqua_client", "Aqua audio sharing client");

    options.add_options()
        ("s,server-ip", "Server IP address", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("p,server-rpc-port", "Server gRPC port", cxxopts::value<std::string>()->default_value("50051"))
        ("u,server-udp-port", "Server UDP media port", cxxopts::value<std::string>()->default_value("50000"))
        ("h,help", "Print usage")
        ("v,version", "Print version");

    ClientCliResult result;
    try {
        auto parsed = options.parse(argc, argv);

        if (parsed.count("help") > 0) {
            std::ostringstream oss;
            oss << options.help();
            result.show_help = true;
            result.help_message = oss.str();
            result.success = true;
            return result;
        }

        if (parsed.count("version") > 0) {
            result.show_version = true;
            result.success = true;
            return result;
        }

        result.server_ip = parsed["server-ip"].as<std::string>();

        auto rpc_port = parse_port(parsed["server-rpc-port"].as<std::string>(), "--server-rpc-port",
                                   result.error_message);
        if (!rpc_port.has_value()) {
            return result;
        }
        result.server_rpc_port = rpc_port.value();

        auto udp_port = parse_port(parsed["server-udp-port"].as<std::string>(), "--server-udp-port",
                                   result.error_message);
        if (!udp_port.has_value()) {
            return result;
        }
        result.server_udp_port = udp_port.value();

        result.success = true;
        return result;

    } catch (const cxxopts::exceptions::exception& e) {
        result.error_message = std::string("Argument parse error: ") + e.what();
        return result;
    }
}

ClientCliResult parse_client_command_line(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("aqua_client");
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    return parse_client_command_line(static_cast<int>(argv.size()), argv.data());
}

} // namespace aqua
