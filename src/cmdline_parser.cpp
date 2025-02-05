//
// Created by aquawius on 25-1-8.
//

#include <iostream>
#include <cxxopts.hpp>
#include "config.h"
#include "cmdline_parser.h"
#include "network_server.h"
#include <spdlog/fmt/fmt.h>

aqua::cmdline_parser::cmdline_parser(int argc, const char* argv[])
    : m_options(aqua_core_BINARY_NAME, get_help_string())
    , m_argc(argc)
    , m_argv(argv)
{
    // clang-format off
    m_options.add_options()
        ("h,help", "Print usage")
        ("b,bind", "Server bind address (IP). Default uses auto-detected private address",
            cxxopts::value<std::string>())
        ("p,port", "Server port (default: 10120)",
            cxxopts::value<uint16_t>()->default_value("10120"))
        ("V,verbose", "Set log level (Not set=info, V=debug, VV=trace)")  // 作为 boolean 选项
        ("v,version", "Show version");
    // clang-format on
}

aqua::cmdline_parser::parse_result aqua::cmdline_parser::parse()
{
    try {
        parse_result result;
        auto parsed = m_options.parse(m_argc, m_argv);

        result.help = parsed.count("help") > 0;
        result.version = parsed.count("version") > 0;

        // 处理日志级别
        int verbose_count = parsed.count("verbose");
        switch (verbose_count) {
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

        // 处理绑定地址
        if (parsed.count("bind")) {
            result.bind_address = parsed["bind"].as<std::string>();
        }

        // 处理端口
        if (parsed.count("port")) {
            result.port = parsed["port"].as<uint16_t>();
        }

        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse command line: ") + e.what());
    }
}

std::string aqua::cmdline_parser::get_help_string()
{
    std::string default_address = network_server::get_default_address();

    std::string help_string = fmt::format("{} - Audio streaming server\n\n", aqua_core_BINARY_NAME);
    help_string += "Usage:\n";
    help_string += fmt::format("  {} [options]\n\n", aqua_core_BINARY_NAME);
    help_string += "Examples:\n";
    help_string += fmt::format("  {} -b 0.0.0.0 -p 10120\n", aqua_core_BINARY_NAME);
    help_string += fmt::format("  {} -V          # Enable debug logging\n", aqua_core_BINARY_NAME);
    help_string += fmt::format("  {} -VV         # Enable trace logging\n\n", aqua_core_BINARY_NAME);
    help_string += fmt::format("Default bind address: {}\n", default_address.empty() ? "0.0.0.0" : default_address);

    return help_string;
}
