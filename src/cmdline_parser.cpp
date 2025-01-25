//
// Created by aquawius on 25-1-8.
//
#include <iostream>
#include <regex>

#include <cxxopts.hpp>

#include "config.h"
#include "cmdline_parser.h"

#include "network_server.h"

#include <spdlog/fmt/fmt.h>

using string = std::string;

aqua::cmdline_parser::cmdline_parser(int argc, const char* argv[])
    : m_options(cxxopts::Options(aqua_core_BINARY_NAME, get_help_string()))
    , m_argc(argc)
    , m_argv(argv)
{
    // clang-format off
    m_options.add_options()
        ("h,help", "Print usage")
        ("l,list-endpoint", "List available endpoints")
        ("b,bind", "The server bind address. If not set, will use default",
            cxxopts::value<string>()->implicit_value(network_server::get_default_address()), "[host][:<port>]")
        ("e,endpoint", "Specify the endpoint id. If not set or set \"default\", will use default",
            cxxopts::value<string>()->default_value("default"), "[endpoint]")
        ("list-encoding", "List available encoding")
        ("channels", "Specify the capture channels. If not set or set \"0\", will use default",
            cxxopts::value<int>()->default_value("0"), "[channels]")
        ("sample-rate", "Specify the capture sample rate(Hz). If not set or set \"0\", will use default",
            cxxopts::value<int>()->default_value("0"), "[sample_rate]")
        ("V,verbose", "Set log level to \"trace\"")
        ("v,version", "Show version");
    // clang-format on
}

std::string aqua::cmdline_parser::parse_address_and_port(const std::string& addr_str, uint16_t& port)
{
    std::regex addr_regex("([^:]+)(?::([0-9]+))?");
    std::smatch matches;

    if (std::regex_match(addr_str, matches, addr_regex)) {
        std::string address = matches[1].str();

        if (matches[2].matched) {
            try {
                port = static_cast<uint16_t>(std::stoi(matches[2].str()));
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid port number");
            }
        }

        return address;
    }

    return addr_str;
}

aqua::cmdline_parser::parse_result aqua::cmdline_parser::parse()
{
    try {
        parse_result result;
        auto parsed = m_options.parse(m_argc, m_argv);

        result.help = parsed.count("help") > 0;
        result.version = parsed.count("version") > 0;
        result.list_endpoint = parsed.count("list-endpoint") > 0;
        result.list_encoding = parsed.count("list-encoding") > 0;
        result.verbose = parsed.count("verbose") > 0;

        if (parsed.count("bind")) {
            std::string bind_str = parsed["bind"].as<std::string>();
            result.bind_address = parse_address_and_port(bind_str, result.port);
        }

        result.endpoint = parsed["endpoint"].as<std::string>();
        result.channels = parsed["channels"].as<int>();
        result.sample_rate = parsed["sample-rate"].as<int>();

        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to parse command line: ") + e.what());
    }
}

std::string aqua::cmdline_parser::get_help_string()
{
    std::string default_address = network_server::get_default_address();
    std::string example_addr = default_address.empty() ? "192.168.3.2" : default_address;

    std::string help_string("Hello World!\n");
    help_string += fmt::format("Example:\n");
    help_string += fmt::format("\t{} -b\n", aqua_core_BINARY_NAME);
    help_string += fmt::format("\t{} --bind={}:12345\n", aqua_core_BINARY_NAME, example_addr);
    help_string += fmt::format("\t{} --bind={}:12345 --encoding=f32 --channels=2 --sample-rate=48000\n",
        aqua_core_BINARY_NAME, example_addr);
    help_string += fmt::format("\t{} -l\n", aqua_core_BINARY_NAME);
    help_string += fmt::format("\t{} --list-encoding\n", aqua_core_BINARY_NAME);

    return help_string;
}
