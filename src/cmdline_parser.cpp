//
// Created by aquawius on 25-2-6.
//

#include "cmdline_parser.h"

#include "version.h"
#include <spdlog/fmt/fmt.h>
#include <stdexcept>

namespace aqua_client {

cmdline_parser::cmdline_parser(int argc, const char* argv[])
    : m_options(aqua_client_BINARY_NAME, get_help_string())
    , m_argc(argc)
    , m_argv(argv)
{
    // clang-format off
    m_options.add_options()
        ("h,help", "Print usage")
        ("s,server", "Server address (required)", cxxopts::value<std::string>())
        ("p,server-port", "Server port (default: 10120)",
         cxxopts::value<uint16_t>()->default_value("10120"))
        ("c,client-address", "Client bind address",
         cxxopts::value<std::string>()->default_value(network_client::get_default_address()))
        ("l,client-port", "Client port (0=random)",
         cxxopts::value<uint16_t>()->default_value("0"))
        ("V,verbose", "Set log level (Not set=info, V=debug, VV=trace)")
        ("v,version", "Show version");
    // clang-format on
}

cmdline_parser::parse_result cmdline_parser::parse()
{
    try {
        parse_result result;
        auto parsed = m_options.parse(m_argc, m_argv);

        result.help = parsed.count("help") > 0;
        result.version = parsed.count("version") > 0;

        // 如果请求帮助或版本，直接返回，不检查其他参数
        if (result.help || result.version) {
            return result;
        }

        // 处理必须参数（仅在非帮助/版本模式下检查）
        if (!parsed.count("server")) {
            throw std::runtime_error("Server address is required");
        }
        result.server_address = parsed["server"].as<std::string>();

        // 处理日志级别
        switch (parsed.count("verbose")) {
        case 0:
            result.log_level = spdlog::level::info;
            break;
        case 1:
            result.log_level = spdlog::level::debug;
            break;
        default:
            result.log_level = spdlog::level::trace;
            break;
        }

        // 其他参数
        result.server_port = parsed["server-port"].as<uint16_t>();
        result.client_address = parsed["client-address"].as<std::string>();
        result.client_port = parsed["client-port"].as<uint16_t>();

        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Command line error: ") + e.what());
    }
}

std::string cmdline_parser::get_help_string()
{
    std::string default_address = network_client::get_default_address();

    std::string help = fmt::format("{} - Audio streaming client\n\n", aqua_client_BINARY_NAME);
    help += "Usage:\n";
    help += fmt::format("  {} -s SERVER_IP [options]\n\n", aqua_client_BINARY_NAME);
    help += "Required:\n";
    help += "  -s, --server       Server IP address\n\n";
    help += "Options:\n";
    help += "  -p, --server-port  Server port (default: 10120)\n";
    help += "  -c, --client-addr  Client bind address (default: "
        + (default_address.empty() ? "auto" : default_address) + ")\n";
    help += "  -l, --client-port  Client port (0=random, default: 0)\n";
    help += "  -V                 Increase log verbosity (-V=debug, -VV=trace)\n";
    help += "  -v, --version      Show version\n";
    help += "  -h, --help         Show this help\n\n";
    help += "Examples:\n";
    help += fmt::format("  {} -s 192.168.1.100 -p 20220\n", aqua_client_BINARY_NAME);
    help += fmt::format("  {} -s 127.0.0.1 -c 0.0.0.0 -VV\n", aqua_client_BINARY_NAME);

    return help;
}
}
