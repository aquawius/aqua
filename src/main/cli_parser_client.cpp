#include "cli_parser_client.h"
#include "cli_parser_common.h"

#include <cxxopts.hpp>
#include <sstream>

namespace aqua {

ClientCliResult parse_client_command_line(int argc, const char* const* argv) {
    cxxopts::Options options("aqua_client", "Aqua audio sharing client");

    // 不接受任何位置参数：所有参数必须是 --option 形式。
    options.positional_help("");
    options.parse_positional({});

    options.add_options()
        ("s,server-ip", "Server IP address", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("p,server-rpc-port", "Server gRPC port", cxxopts::value<std::string>()->default_value("50051"))
        ("j,jitter-latency", "JitterBuffer target latency in ms (recommend: 30 for WiFi, 15 for wired LAN)", cxxopts::value<uint32_t>()->default_value("30"))
        ("l,log-level", "Log level: trace/debug/info/warn/error (default: info)", cxxopts::value<std::string>())
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

        // 拒绝任何未匹配的位置参数。
        // 所有配置必须通过 --option 显式指定，默认值见 --help。
        if (!parsed.unmatched().empty()) {
            result.error_message = "Unknown argument(s): "
                                 + parsed.unmatched()[0]
                                 + "\nUse --help to see usage.";
            return result;
        }

        result.server_ip = parsed["server-ip"].as<std::string>();

        auto rpc_port = parse_port(parsed["server-rpc-port"].as<std::string>(), "--server-rpc-port",
                                   result.error_message);
        if (!rpc_port.has_value()) {
            return result;
        }
        result.server_rpc_port = rpc_port.value();

        result.jitter_latency_ms = parsed["jitter-latency"].as<uint32_t>();

        if (parsed.count("log-level") > 0) {
            auto lvl = log_level_from_string(parsed["log-level"].as<std::string>());
            if (!lvl) {
                result.error_message = "Invalid --log-level '" + parsed["log-level"].as<std::string>()
                                     + "' (expected: trace/debug/info/warn/error)";
                return result;
            }
            result.log_level = *lvl;
        }

        result.success = true;
        return result;

    } catch (const cxxopts::exceptions::exception& e) {
        result.error_message = std::string("Argument parse error: ") + e.what()
                             + "\nUse --help to see usage.";
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
