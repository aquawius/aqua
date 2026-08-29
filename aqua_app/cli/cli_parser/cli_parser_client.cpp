#include "cli_parser_client.h"

#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"

#include <cxxopts.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace aqua::cli {
namespace {

void print_output_devices(const aqua::audio::AudioDeviceManager& manager)
{
    const auto devices = manager.enumerate(aqua::audio::AudioDeviceDirection::OUTPUT);
    std::cout << "[OUTPUT] devices (" << devices.size() << ")\n";
    for (const auto& device : devices) {
        std::cout << "  " << (device.is_default ? "* " : "  ")
                  << device.name << "\n"
                  << "      id: " << device.id.value() << "\n";
        const auto format = manager.default_format(aqua::audio::AudioDeviceDirection::OUTPUT, device.id);
        if (format) {
            std::cout << "      default format: " << format->channels << "ch/"
                      << format->sample_rate << "Hz/" << audio_encoding_name(format->encoding) << "\n";
        } else {
            std::cout << "      default format: unavailable ("
                      << aqua::audio::audio_error_name(format.error()) << ")\n";
        }
    }
}

} // namespace

ParseOutcome parse_client_cli(int argc, char** argv, runtime::ClientRuntimeConfig& config, LogLevel& log_level)
{
    cxxopts::Options options("aqua_client", "Aqua audio client (gRPC control + UDP data plane)");
    options.add_options()
        ("server-ip", "server IP (required)", cxxopts::value<std::string>())
        ("server-rpc", "server gRPC port (required)", cxxopts::value<std::uint16_t>())
        ("name", "client name", cxxopts::value<std::string>()->default_value("aqua-client"))
        ("jitter-slots", "jitter buffer slot count (4..4096)", cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::DEFAULT_CLIENT_JITTER_BUFFER_SLOTS)))
        ("device-id", "playback output device id (omit for system default)", cxxopts::value<std::string>())
        ("log-level", "log level: trace|debug|info|warn|error|fatal", cxxopts::value<std::string>()->default_value("info"))
        ("list-devices", "list active output audio devices and exit", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            std::cout << options.help() << '\n';
            return ParseOutcome::Help;
        }

        if (result["list-devices"].as<bool>()) {
            auto manager = audio::create_device_manager();
            if (!manager) {
                std::cerr << "audio device enumeration is unavailable on this platform\n";
                return ParseOutcome::Error;
            }
            print_output_devices(*manager);
            return ParseOutcome::ListDevices;
        }

        if (result.count("server-ip") == 0) {
            std::cerr << "missing required option --server-ip\n";
            return ParseOutcome::Error;
        }
        if (result.count("server-rpc") == 0) {
            std::cerr << "missing required option --server-rpc\n";
            return ParseOutcome::Error;
        }

        config.jitter_buffer_slots = result["jitter-slots"].as<std::uint32_t>();
        config.server_ip = result["server-ip"].as<std::string>();
        config.rpc_port = result["server-rpc"].as<std::uint16_t>();
        config.client_name = result["name"].as<std::string>();

        if (config.jitter_buffer_slots < aqua::config::MIN_JITTER_BUFFER_SLOTS
            || config.jitter_buffer_slots > kMaxJitterBufferSlots) {
            std::cerr << "invalid --jitter-slots: expected " << aqua::config::MIN_JITTER_BUFFER_SLOTS
                      << ".." << kMaxJitterBufferSlots << "\n";
            return ParseOutcome::Error;
        }
        if (config.server_ip.empty()) {
            std::cerr << "invalid --server-ip: value must not be empty\n";
            return ParseOutcome::Error;
        }
        try {
            const auto server_address = ::aqua::net::parse_ip_address(config.server_ip);
            if (server_address.is_unspecified()) {
                std::cerr << "invalid --server-ip: address must be a concrete reachable IP\n";
                return ParseOutcome::Error;
            }
        } catch (const std::exception& e) {
            std::cerr << "invalid --server-ip: " << e.what() << "\n";
            return ParseOutcome::Error;
        }
        if (config.rpc_port == 0) {
            std::cerr << "invalid --server-rpc: must be > 0\n";
            return ParseOutcome::Error;
        }
        if (config.client_name.empty() || config.client_name.size() > aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) {
            std::cerr << "invalid --name: expected 1.." << aqua::config::GRPC_MAX_CLIENT_NAME_BYTES << " bytes\n";
            return ParseOutcome::Error;
        }
        const auto parsed_log_level = aqua::string_to_log_level_enum(result["log-level"].as<std::string>());
        if (!parsed_log_level) {
            std::cerr << "invalid --log-level: expected trace|debug|info|warn|error|fatal\n";
            return ParseOutcome::Error;
        }
        log_level = *parsed_log_level;
        if (result.count("device-id") != 0) {
            const auto id = result["device-id"].as<std::string>();
            if (id.empty()) {
                std::cerr << "invalid --device-id: value must not be empty\n";
                return ParseOutcome::Error;
            }
            config.playback.device = audio::AudioDeviceId(id);
        } else {
            config.playback.device.reset();
        }

        return ParseOutcome::Run;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return ParseOutcome::Error;
    }
}

} // namespace aqua::cli
