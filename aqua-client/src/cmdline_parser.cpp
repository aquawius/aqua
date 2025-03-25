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
        result.server_rpc_port = parsed["server-port"].as<uint16_t>();
        result.client_address = parsed["client-address"].as<std::string>();
        result.client_udp_port = parsed["client-port"].as<uint16_t>();

        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Command line error: ") + e.what());
    }
}

std::string cmdline_parser::get_help_string()
{
    std::string default_address = network_client::get_default_address();

    std::string help_string;

    // 程序名称和描述
    help_string += fmt::format("{} - Audio Streaming Client\n", aqua_client_BINARY_NAME);
    help_string += "Connect to audio streaming server and play received audio\n\n";

    // 基本用法
    help_string += "USAGE:\n";
    help_string += fmt::format("  {} -s SERVER_IP [OPTIONS]\n\n", aqua_client_BINARY_NAME);

    // 必需选项
    help_string += "REQUIRED:\n";
    help_string += "  -s, --server <address>  Server IP address to connect\n\n";

    // 选项分组 - 网络设置
    help_string += "NETWORK OPTIONS:\n";
    help_string += "  -p, --server-port <port> Server port number\n";
    help_string += "                         Default: 10120\n";
    help_string += "  -c, --client-address    Client bind address\n";
    help_string += fmt::format("                         Default: {}\n", default_address.empty() ? "auto" : default_address);
    help_string += "  -l, --client-port <port> Client UDP port\n";
    help_string += "                         Default: 0 (random port 49152-65535)\n";
    help_string += "Will send `client address/port` to server through RPC, Server\n     should send audio data to `THIS` endpoint.\n\n";

    // 选项分组 - 其他选项
    help_string += "OTHER OPTIONS:\n";
    help_string += "  -V, --verbose           Increase logging verbosity\n";
    help_string += "                         Not set = info, -V = debug, -VV = trace\n";
    help_string += "  -h, --help              Display this help message\n";
    help_string += "  -v, --version           Display version information\n\n";

    // 使用示例
    help_string += "EXAMPLES:\n";
    help_string += fmt::format("  # Connect to local server with default settings\n  {} -s 127.0.0.1\n\n",
        aqua_client_BINARY_NAME);
    help_string += fmt::format("  # Connect to remote server with specific ports\n  {} -s 192.168.1.100 -p 8080 -l 8081\n\n",
        aqua_client_BINARY_NAME);
    help_string += fmt::format("  # Connect with specific client address and debug logging\n  {} -s 192.168.1.100 -c 0.0.0.0 -V\n\n",
        aqua_client_BINARY_NAME);

    // 注意事项
    help_string += "NOTES:\n";
    help_string += "  - Audio format will be automatically configured based on server settings\n";
    help_string += "  - Random client port will be used if not specified\n";
    help_string += "  - Client will automatically try reconnect on connection loss\n";

    return help_string;
}
}