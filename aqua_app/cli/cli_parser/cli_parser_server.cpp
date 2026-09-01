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
        ("server-ip", "Local IP address to bind for both gRPC control and UDP data plane; use 0.0.0.0 to listen on all IPv4 interfaces or :: for all IPv6 interfaces.",
            cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_BIND_IP))
        ("rpc-port", "TCP port for the gRPC control plane, where clients connect to start and stop sessions.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultRpcPort)))
        ("udp-port", "UDP port for the audio data plane, which carries audio frames and the HELLO keepalive.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultUdpPort)))
        ("advertise-ip", "UDP IP address sent to clients as the data-plane destination. Defaults to the same address as --server-ip; set it only when clients cannot reach the bind address directly (NAT, containers, or multi-homed hosts).",
            cxxopts::value<std::string>())
        ("advertise-udp-port", "UDP port sent to clients as the data-plane destination. Defaults to the same port as --udp-port; set it only when a NAT or port-forward maps the external port to a different one.",
            cxxopts::value<std::uint16_t>())
        ("encoding", "PCM sample encoding for the stream: s16, s24, s32, f32, or u8. Must be set together with --channels and --sample-rate; omit all three to use the capture device's default format.",
            cxxopts::value<std::string>())
        ("channels", "Number of audio channels in the stream. Must be set together with --encoding and --sample-rate; omit all three to use the capture device's default format.",
            cxxopts::value<std::uint32_t>())
        ("sample-rate", "Sample rate in Hz for the stream. Must be set together with --encoding and --channels; omit all three to use the capture device's default format.",
            cxxopts::value<std::uint32_t>())
        ("frames-per-slot", "Number of sample frames packed into each UDP audio packet. 0 means the server picks the largest value that fits in one IPv6-safe packet; explicit values must be at least 16.",
            cxxopts::value<std::uint32_t>()->default_value("0"))
        ("capture", "What the server captures: loopback records the system OUTPUT mix, input records from a microphone or INPUT device.",
            cxxopts::value<std::string>()->default_value("loopback"))
        ("device-id", "Capture device ID to use instead of the system default. Must match the --capture direction (an OUTPUT device for loopback, an INPUT device for input); list available IDs with --list-devices.",
            cxxopts::value<std::string>())
        ("session-timeout-ms", "How long a client may stay silent (no HELLO keepalive) before the server considers its session gone and removes it.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_TIMEOUT.count())))
        ("reap-interval-ms", "How often the server scans for sessions that have been silent longer than --session-timeout-ms.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_REAP_INTERVAL.count())))
        ("network-queue-slots", "Capacity of the buffer between audio capture and the network sender, measured in frames. A larger value absorbs capture/dispatch timing hiccups without adding steady-state latency; it only adds delay if the buffer actually fills up.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::DEFAULT_SERVER_NETWORK_QUEUE_SLOTS)))
        ("log-level", "Verbosity of log output: trace, debug, info, warn, error, or fatal.",
            cxxopts::value<std::string>()->default_value(aqua::log_level_name(aqua::default_log_level())))
        ("list-devices", "List available INPUT and OUTPUT audio devices with their IDs and default formats, then exit.",
            cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print this help text and exit.");

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

        const auto server_ip = result["server-ip"].as<std::string>();
        if (!validate_ip_literal(server_ip, "--server-ip")) {
            return ParseOutcome::Error;
        }
        if (result["rpc-port"].as<std::uint16_t>() == 0) {
            std::cerr << "invalid --rpc-port: must be > 0\n";
            return ParseOutcome::Error;
        }
        if (result["session-timeout-ms"].as<std::uint32_t>() == 0) {
            std::cerr << "invalid --session-timeout-ms: must be > 0\n";
            return ParseOutcome::Error;
        }
        if (result["reap-interval-ms"].as<std::uint32_t>() == 0) {
            std::cerr << "invalid --reap-interval-ms: must be > 0\n";
            return ParseOutcome::Error;
        }

        config.frame_count = requested_fps;
        config.server_ip = server_ip;
        config.udp_port = result["udp-port"].as<std::uint16_t>();
        if (config.udp_port == 0) {
            std::cerr << "invalid --udp-port: must be > 0\n";
            return ParseOutcome::Error;
        }
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
            if (auto manager = audio::create_device_manager()) {
                const auto direction = config.capture.source == audio::AudioCaptureSource::INPUT_DEVICE
                    ? audio::AudioDeviceDirection::INPUT
                    : audio::AudioDeviceDirection::OUTPUT;
                const auto resolved = manager->resolve(direction, config.capture.device);
                if (!resolved) {
                    const char* expected = direction == audio::AudioDeviceDirection::INPUT ? "INPUT" : "OUTPUT (loopback)";
                    std::cerr << "invalid --device-id: cannot resolve the specified " << expected
                              << " capture endpoint (device may not exist or has the wrong direction)\n";
                    return ParseOutcome::Error;
                }
            }
        } else {
            config.capture.device.reset();
        }
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        // 默认通告地址/端口跟随 server_ip / udp_port；显式 advertise 参数用于部署在
        // NAT、容器或多网卡环境时指定 client 实际可达的数据面 endpoint。
        if (result.count("advertise-ip") != 0) {
            config.advertised_udp_address = result["advertise-ip"].as<std::string>();
            if (!validate_ip_literal(config.advertised_udp_address, "--advertise-ip")) {
                return ParseOutcome::Error;
            }
        } else {
            config.advertised_udp_address.clear();
        }
        if (result.count("advertise-udp-port") != 0) {
            const auto port = result["advertise-udp-port"].as<std::uint16_t>();
            if (port == 0) {
                std::cerr << "invalid --advertise-udp-port: must be > 0\n";
                return ParseOutcome::Error;
            }
            config.advertised_udp_port = port;
        } else {
            config.advertised_udp_port.reset();
        }
        const auto parsed_log_level = aqua::string_to_log_level_enum(result["log-level"].as<std::string>());
        if (!parsed_log_level) {
            std::cerr << "invalid --log-level: expected trace|debug|info|warn|error|fatal\n";
            return ParseOutcome::Error;
        }
        log_level = *parsed_log_level;

        return ParseOutcome::Run;
    } catch (const std::exception& e) {
        std::cerr << format_exception_message(e) << '\n';
        return ParseOutcome::Error;
    }
}

} // namespace aqua::cli
