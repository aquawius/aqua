#include "cli_parser_server.h"

#include <cxxopts.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace aqua::cli {

ParseOutcome parse_server_cli(int argc, char** argv, runtime::ServerRuntimeConfig& config, LogLevel& log_level)
{
    cxxopts::Options options("aqua_server", "Aqua audio server (gRPC control + UDP data plane)");
    options.add_options()
        ("rpc-ip", "gRPC bind IP", cxxopts::value<std::string>()->default_value("0.0.0.0"))
        ("rpc-port", "gRPC port", cxxopts::value<std::uint16_t>()->default_value("50051"))
        ("udp-ip", "UDP bind IP", cxxopts::value<std::string>()->default_value("0.0.0.0"))
        ("udp-port", "UDP data plane port", cxxopts::value<std::uint16_t>()->default_value("9999"))
        ("advertise-ip", "UDP IP advertised to clients", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("encoding", "PCM encoding: s16|s24|s32|f32|u8", cxxopts::value<std::string>()->default_value("f32"))
        ("channels", "channel count", cxxopts::value<std::uint32_t>()->default_value("2"))
        ("sample-rate", "sample rate (Hz)", cxxopts::value<std::uint32_t>()->default_value("48000"))
        ("frames-per-slot", "frames per AudioFrame (0=auto from MTU)", cxxopts::value<std::uint32_t>()->default_value("0"))
        ("capture", "capture source: loopback|input", cxxopts::value<std::string>()->default_value("loopback"))
        ("device-id", "specific device id", cxxopts::value<std::string>())
        ("session-timeout-ms", "session timeout (ms)", cxxopts::value<std::uint32_t>()->default_value("5000"))
        ("reap-interval-ms", "session reap interval (ms)", cxxopts::value<std::uint32_t>()->default_value("1000"))
        ("network-queue-slots", "capture to network handoff slots", cxxopts::value<std::uint32_t>()->default_value("4"))
        ("log-level", "log level: trace|debug|info|warn|error|fatal", cxxopts::value<std::string>()->default_value("info"))
        ("h,help", "print usage");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            std::cout << options.help() << '\n';
            return ParseOutcome::Help;
        }

        const auto enc = parse_encoding(result["encoding"].as<std::string>());
        if (!enc) {
            std::cerr << "invalid --encoding\n";
            return ParseOutcome::Error;
        }
        const auto format = make_format(*enc,
            result["channels"].as<std::uint32_t>(),
            result["sample-rate"].as<std::uint32_t>());
        if (!format.is_valid()) {
            std::cerr << "invalid audio format (channels/rate/encoding out of range)\n";
            return ParseOutcome::Error;
        }
        const auto capture_mode = result["capture"].as<std::string>();
        if (capture_mode != "loopback" && capture_mode != "input") {
            std::cerr << "invalid --capture: expected loopback|input\n";
            return ParseOutcome::Error;
        }
        const auto fps = resolve_frame_count(
            result["frames-per-slot"].as<std::uint32_t>(), format);
        if (fps == 0) {
            std::cerr << "invalid --frames-per-slot: must be > 0 and fit within MTU budget\n";
            return ParseOutcome::Error;
        }

        config.format = format;
        config.frame_count = fps;
        config.udp_bind_ip = result["udp-ip"].as<std::string>();
        config.udp_port = result["udp-port"].as<std::uint16_t>();
        config.session_timeout = std::chrono::milliseconds(result["session-timeout-ms"].as<std::uint32_t>());
        config.session_reap_interval = std::chrono::milliseconds(result["reap-interval-ms"].as<std::uint32_t>());
        config.network_queue_slots = result["network-queue-slots"].as<std::uint32_t>();
        config.capture.source = (capture_mode == "input")
            ? audio::AudioCaptureSource::INPUT_DEVICE
            : audio::AudioCaptureSource::OUTPUT_LOOPBACK;
        if (result.count("device-id") != 0) {
            config.capture.device = audio::AudioDeviceId(result["device-id"].as<std::string>());
        }
        config.rpc_bind_ip = result["rpc-ip"].as<std::string>();
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        config.advertised_udp_address = result["advertise-ip"].as<std::string>();
        const auto parsed_log_level = aqua::string_to_log_level_enum(result["log-level"].as<std::string>());
        if (!parsed_log_level) {
            std::cerr << "invalid --log-level: expected trace|debug|info|warn|error|fatal\n";
            return ParseOutcome::Error;
        }
        log_level = *parsed_log_level;

        return ParseOutcome::Run;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return ParseOutcome::Error;
    }
}

} // namespace aqua::cli
