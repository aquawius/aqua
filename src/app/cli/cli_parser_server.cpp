#include "app/cli/cli_parser_server.h"
#include "app/cli/cli_parser_common.h"

#include <cxxopts.hpp>
#include <sstream>

namespace aqua {

ServerCliResult parse_server_command_line(int argc, const char* const* argv) {
    cxxopts::Options options("aqua_server", "Aqua audio sharing server");

    // 不接受任何位置参数：所有参数必须是 --option 形式。
    // 裸参数（如 "aqua_server 192.168.45.1"）会报错，提示用户查看 --help。
    options.positional_help("");
    options.parse_positional({});

    options.add_options()
        ("b,bind-ip", "Bind IP address", cxxopts::value<std::string>()->default_value("0.0.0.0"))
        ("r,rpc-port", "gRPC port", cxxopts::value<std::string>()->default_value("50051"))
        ("u,udp-port", "UDP media port", cxxopts::value<std::string>()->default_value("50000"))
        ("capture-buffer", "Capture RingBuffer size in bytes (0 = default 8192)", cxxopts::value<long long>()->default_value("0"))
        ("l,log-level", "Log level: trace/debug/info/warn/error (default: debug in debug build, info in release)", cxxopts::value<std::string>())
        ("h,help", "Print usage")
        ("v,version", "Print version");

    ServerCliResult result;
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

        // 拒绝任何未匹配的位置参数（如 "aqua_server 192.168.45.1"）。
        // 所有配置必须通过 --option 显式指定，默认值见 --help。
        if (!parsed.unmatched().empty()) {
            result.error_message = "Unknown argument(s): "
                                 + parsed.unmatched()[0]
                                 + "\nUse --help to see usage.";
            return result;
        }

        result.bind_ip = parsed["bind-ip"].as<std::string>();

        auto rpc_port = parse_port(parsed["rpc-port"].as<std::string>(), "--rpc-port", result.error_message);
        if (!rpc_port.has_value()) {
            return result;
        }
        result.rpc_port = rpc_port.value();

        auto udp_port = parse_port(parsed["udp-port"].as<std::string>(), "--udp-port", result.error_message);
        if (!udp_port.has_value()) {
            return result;
        }
        result.udp_port = udp_port.value();
        // 用 long long 解析避免负数经 std::stoul 变为 ULONG_MAX 后截断溢出。
        // 合理范围 [0, 64MB]：8KB 默认，64MB 足以容纳数秒音频缓冲。
        auto capture_buf = parsed["capture-buffer"].as<long long>();
        if (capture_buf < 0 || capture_buf > 64LL * 1024 * 1024) {
            result.error_message = "--capture-buffer must be in range 0..67108864 (64MB)";
            return result;
        }
        result.capture_buffer_size = static_cast<std::size_t>(capture_buf);

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

ServerCliResult parse_server_command_line(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("aqua_server");
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    return parse_server_command_line(static_cast<int>(argv.size()), argv.data());
}

} // namespace aqua
