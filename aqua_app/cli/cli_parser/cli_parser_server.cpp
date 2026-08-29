#include "cli_parser_server.h"

#include <cxxopts.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace aqua::cli {
namespace {

void print_devices(const aqua::audio::AudioDeviceManager& manager, aqua::audio::AudioDeviceDirection direction)
{
    const auto devices = manager.enumerate(direction);
    const char* label = direction == aqua::audio::AudioDeviceDirection::INPUT ? "INPUT" : "OUTPUT";
    std::cout << "[" << label << "] devices (" << devices.size() << ")\n";
    for (const auto& device : devices) {
        std::cout << "  " << (device.is_default ? "* " : "  ")
                  << device.name << "\n"
                  << "      id: " << device.id.value() << "\n";
        const auto format = manager.default_format(direction, device.id);
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

ParseOutcome parse_server_cli(int argc, char** argv, runtime::ServerRuntimeConfig& config, LogLevel& log_level)
{
    cxxopts::Options options("aqua_server", "Aqua audio server (gRPC control + UDP data plane)");
    options.add_options()
        ("rpc-ip", "gRPC bind IP", cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_BIND_IP))
        ("rpc-port", "gRPC port", cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultRpcPort)))
        ("udp-ip", "UDP bind IP", cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_BIND_IP))
        ("udp-port", "UDP data plane port", cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultUdpPort)))
        ("advertise-ip", "UDP IP advertised to clients (default: --udp-ip; 0.0.0.0/:: = fallback to client --server-ip)", cxxopts::value<std::string>())
        ("encoding", "PCM encoding: s16|s24|s32|f32|u8 (omit all three to use backend default)", cxxopts::value<std::string>())
        ("channels", "channel count (omit all three to use backend default)", cxxopts::value<std::uint32_t>())
        ("sample-rate", "sample rate (Hz; omit all three to use backend default)", cxxopts::value<std::uint32_t>())
        ("frames-per-slot", "frames per AudioFrame (0=auto from MTU, explicit >=16)", cxxopts::value<std::uint32_t>()->default_value("0"))
        ("capture", "capture source: loopback|input", cxxopts::value<std::string>()->default_value("loopback"))
        ("device-id", "capture device id; without --capture it selects an OUTPUT device for loopback; with --capture=input it selects an INPUT device", cxxopts::value<std::string>())
        ("session-timeout-ms", "session timeout (ms)", cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_TIMEOUT.count())))
        ("reap-interval-ms", "session reap interval (ms)", cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_REAP_INTERVAL.count())))
        ("network-queue-slots", "capture to network handoff slots (1..4096)", cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::DEFAULT_SERVER_NETWORK_QUEUE_SLOTS)))
        ("log-level", "log level: trace|debug|info|warn|error|fatal", cxxopts::value<std::string>()->default_value(aqua::log_level_name(aqua::default_log_level())))
        ("list-devices", "list active audio devices and exit", cxxopts::value<bool>()->default_value("false"))
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
            print_devices(*manager, aqua::audio::AudioDeviceDirection::INPUT);
            print_devices(*manager, aqua::audio::AudioDeviceDirection::OUTPUT);
            return ParseOutcome::ListDevices;
        }

        const bool has_encoding = result.count("encoding") != 0;
        const bool has_channels = result.count("channels") != 0;
        const bool has_sample_rate = result.count("sample-rate") != 0;
        const bool any_format_option = has_encoding || has_channels || has_sample_rate;
        if (any_format_option && !(has_encoding && has_channels && has_sample_rate)) {
            std::cerr << "--encoding, --channels and --sample-rate must be specified together; omit all three for backend default\n";
            return ParseOutcome::Error;
        }
        if (any_format_option) {
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
            config.format = format;
        } else {
            config.format.reset();
        }

        const auto capture_mode = result["capture"].as<std::string>();
        if (capture_mode != "loopback" && capture_mode != "input") {
            std::cerr << "invalid --capture: expected loopback|input\n";
            return ParseOutcome::Error;
        }

        const auto requested_fps = result["frames-per-slot"].as<std::uint32_t>();
        if (requested_fps != 0 && requested_fps < kMinFramesPerSlot) {
            std::cerr << "invalid --frames-per-slot: must be 0 (auto) or at least "
                      << kMinFramesPerSlot << "\n";
            return ParseOutcome::Error;
        }

        const auto network_queue_slots = result["network-queue-slots"].as<std::uint32_t>();
        if (network_queue_slots == 0 || network_queue_slots > kMaxNetworkQueueSlots) {
            std::cerr << "invalid --network-queue-slots: expected 1.."
                      << kMaxNetworkQueueSlots << "\n";
            return ParseOutcome::Error;
        }

        config.frame_count = requested_fps;
        config.udp_bind_ip = result["udp-ip"].as<std::string>();
        config.udp_port = result["udp-port"].as<std::uint16_t>();
        config.session_timeout = std::chrono::milliseconds(result["session-timeout-ms"].as<std::uint32_t>());
        config.session_reap_interval = std::chrono::milliseconds(result["reap-interval-ms"].as<std::uint32_t>());
        config.network_queue_slots = network_queue_slots;
        config.capture.source = (capture_mode == "input")
            ? audio::AudioCaptureSource::INPUT_DEVICE
            : audio::AudioCaptureSource::OUTPUT_LOOPBACK;
        if (result.count("device-id") != 0) {
            const auto id = result["device-id"].as<std::string>();
            if (id.empty()) {
                std::cerr << "invalid --device-id: value must not be empty\n";
                return ParseOutcome::Error;
            }
            config.capture.device = audio::AudioDeviceId(id);
        } else {
            config.capture.device.reset();
        }
        config.rpc_bind_ip = result["rpc-ip"].as<std::string>();
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        // By default advertise the same IP used for UDP bind. With the normal wildcard
        // bind this yields 0.0.0.0, which the client interprets as a fallback sentinel.
        if (result.count("advertise-ip") != 0) {
            config.advertised_udp_address = result["advertise-ip"].as<std::string>();
            if (config.advertised_udp_address.empty()) {
                std::cerr << "invalid --advertise-ip: value must not be empty when explicitly specified\n";
                return ParseOutcome::Error;
            }
        } else {
            config.advertised_udp_address.clear(); // Runtime derives it from udp_bind_ip.
        }
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
