#include "cli_parser_client.h"

#include <cxxopts.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace aqua::cli {

ParseOutcome parse_client_cli(int argc, char** argv, runtime::ClientRuntimeConfig& config, LogLevel& log_level)
{
    cxxopts::Options options("aqua_client", "Aqua audio client (gRPC control + UDP data plane)");
    options.add_options()
        ("server-ip", "server IP", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("rpc-port", "server gRPC port", cxxopts::value<std::uint16_t>()->default_value("50051"))
        ("name", "client name", cxxopts::value<std::string>()->default_value("aqua-client"))
        ("jitter-slots", "jitter buffer slot count", cxxopts::value<std::uint32_t>()->default_value("30"))
        ("device-id", "playback device id", cxxopts::value<std::string>())
        ("log-level", "log level: trace|debug|info|warn|error|fatal", cxxopts::value<std::string>()->default_value("info"))
        ("h,help", "print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            std::cout << options.help() << '\n';
            return ParseOutcome::Help;
        }

        config.jitter_buffer_slots = result["jitter-slots"].as<std::uint32_t>();
        config.server_ip = result["server-ip"].as<std::string>();
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        config.client_name = result["name"].as<std::string>();
        const auto parsed_log_level = aqua::string_to_log_level_enum(result["log-level"].as<std::string>());
        if (!parsed_log_level) {
            std::cerr << "invalid --log-level: expected trace|debug|info|warn|error|fatal\n";
            return ParseOutcome::Error;
        }
        log_level = *parsed_log_level;
        if (result.count("device-id") != 0) {
            config.playback.device = audio::AudioDeviceId(result["device-id"].as<std::string>());
        }

        return ParseOutcome::Run;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return ParseOutcome::Error;
    }
}

} // namespace aqua::cli
