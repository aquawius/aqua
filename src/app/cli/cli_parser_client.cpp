#include "app/cli/cli_parser_client.h"
#include "app/cli/cli_parser_common.h"

#include <cxxopts.hpp>
#include <sstream>

namespace aqua {

ClientCliResult parse_client_command_line(int argc, const char* const* argv) {
    cxxopts::Options options("aqua_client", "Aqua audio sharing client");

    // 不接受任何位置参数：所有参数必须是 --option 形式。
    options.positional_help("");
    options.parse_positional({});

    // 注意：数值选项使用 long long 而非 uint32_t/std::size_t，
    // 避免负数经 std::stoul 解析为 ULONG_MAX 后截断溢出。
    options.add_options()
        ("s,server-ip", "Server IP address", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("p,server-rpc-port", "Server gRPC port", cxxopts::value<std::string>()->default_value("50051"))
        ("jitter-latency", "JitterBuffer target latency floor in ms (0 = default 30)", cxxopts::value<long long>()->default_value("0"))
        ("jitter-max-latency", "JitterBuffer adaptive target ceiling in ms (0 = follow capacity/2; enables full adaptive range [floor, ceiling])", cxxopts::value<long long>()->default_value("0"))
        ("jitter-adapt-window", "JitterBuffer adaptive evaluation window in packets (0 = default 500)", cxxopts::value<long long>()->default_value("0"))
        ("drift-threshold", "JitterBuffer drift late threshold in packets per window (0 = default 15)", cxxopts::value<long long>()->default_value("0"))
        ("playback-buffer", "Playback RingBuffer size in bytes (0 = default 16384)", cxxopts::value<long long>()->default_value("0"))
        ("auto-reconnect", "Auto-reconnect to server with exponential backoff (default: off)")
        ("l,log-level", "Log level: trace/debug/info/warn/error (default: debug in debug build, info in release)", cxxopts::value<std::string>())
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

        // 负数经 long long 解析后可正常识别为负值，此处校验范围后再赋值。
        // jitter-latency 合理范围 [0, 1000] ms
        const long long jitter_latency = parsed["jitter-latency"].as<long long>();
        if (jitter_latency < 0 || jitter_latency > 1000) {
            result.error_message = "--jitter-latency must be in range 0..1000 (ms)";
            return result;
        }
        result.jitter_latency_ms = static_cast<uint32_t>(jitter_latency);

        // jitter-max-latency 合理范围 [0, 1000] ms
        const long long jitter_max_latency = parsed["jitter-max-latency"].as<long long>();
        if (jitter_max_latency < 0 || jitter_max_latency > 1000) {
            result.error_message = "--jitter-max-latency must be in range 0..1000 (ms)";
            return result;
        }
        result.jitter_max_latency_ms = static_cast<uint32_t>(jitter_max_latency);

        // jitter-adapt-window 合理范围 [0, 10000] 包
        const long long adapt_window = parsed["jitter-adapt-window"].as<long long>();
        if (adapt_window < 0 || adapt_window > 10000) {
            result.error_message = "--jitter-adapt-window must be in range 0..10000 (packets)";
            return result;
        }
        result.jitter_adapt_window_packets = static_cast<uint32_t>(adapt_window);

        // drift-threshold 合理范围 [0, 10000]
        const long long drift_threshold = parsed["drift-threshold"].as<long long>();
        if (drift_threshold < 0 || drift_threshold > 10000) {
            result.error_message = "--drift-threshold must be in range 0..10000";
            return result;
        }
        result.drift_late_threshold = static_cast<uint32_t>(drift_threshold);

        // playback-buffer 合理范围 [0, 64MB]
        constexpr long long MAX_PLAYBACK_BUFFER = 64LL * 1024 * 1024;
        const long long playback_buffer = parsed["playback-buffer"].as<long long>();
        if (playback_buffer < 0 || playback_buffer > MAX_PLAYBACK_BUFFER) {
            result.error_message = "--playback-buffer must be in range 0..67108864 (64MB)";
            return result;
        }
        result.playback_buffer_size = static_cast<std::size_t>(playback_buffer);

        result.auto_reconnect = parsed.count("auto-reconnect") > 0;

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
