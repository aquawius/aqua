#include "cli_parser_server.h"

#include <aqua_app/cli/cli_version.h>

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
    // clang-format off: cxxopts 选项链刻意一选项一行；formatter 的 BinPack 输出不可读。
    options.add_options()
        ("server-ip", "Local IP address to bind for both gRPC control and UDP data plane; use 0.0.0.0 to listen on all IPv4 interfaces or :: for all IPv6 interfaces. It is also the UDP address advertised to clients unless --udp-advertise-ip overrides it.",
            cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_BIND_IP))
        ("rpc-port", "TCP port (1..65535) for the gRPC control plane, where clients connect to start and stop sessions and where the server advertises its UDP endpoint. Default 50051.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultRpcPort)))
        ("udp-port", "UDP port (1..65535) for the audio data plane, which carries audio frames and the Heartbeat keepalive. Clients learn this port over gRPC; use --udp-advertise-port when a NAT maps it to something else. Default 50000.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultUdpPort)))
        ("udp-advertise-ip", "UDP IP address sent to clients as the data-plane destination. Defaults to the same address as --server-ip; set it only when clients cannot reach the bind address directly (NAT, containers, or multi-homed hosts).",
            cxxopts::value<std::string>())
        ("udp-advertise-port", "UDP port (1..65535) sent to clients as the data-plane destination. Defaults to the same port as --udp-port; set it only when a NAT or port-forward maps the external port to a different one.",
            cxxopts::value<std::uint16_t>())
        ("audio-encoding", "PCM sample encoding for the stream; allowed values: s16|s24|s32|f32|u8. Must be set together with --audio-channels and --audio-sample-rate; omit all three to use the capture device's default format.",
            cxxopts::value<std::string>())
        ("audio-channels", "Number of audio channels in the stream. Must be set together with --audio-encoding and --audio-sample-rate; omit all three to use the capture device's default format.",
            cxxopts::value<std::uint32_t>())
        ("audio-sample-rate", "Sample rate in Hz for the stream. Must be set together with --audio-encoding and --audio-channels; omit all three to use the capture device's default format.",
            cxxopts::value<std::uint32_t>())
        ("audio-packet-frames", "Number of sample frames F packed into each UDP audio packet. 0 = auto (the largest value that fits in one IPv6-safe packet, typically 175 at 2ch/48kHz/s32); otherwise 16 or more, and it must still fit in one packet. F is the latency/packet-rate trade-off: it is both the granularity of the client jitter buffer and how often the server sends - halving F halves per-packet latency but doubles packets/s and makes the stream more sensitive to loss. F is fixed for the lifetime of the server.",
            cxxopts::value<std::uint32_t>()->default_value("0"))
        ("capture", "What the server captures; allowed values: loopback|input. loopback (default) records the system OUTPUT mix, so it needs an OUTPUT device; input records from a microphone or INPUT device. The choice also fixes which direction --capture-device-id resolves in.",
            cxxopts::value<std::string>()->default_value("loopback"))
        ("capture-device-id", "Capture device ID to use instead of the system default. Must match the --capture direction (an OUTPUT device for loopback, an INPUT device for input); list available IDs with --list-devices.",
            cxxopts::value<std::string>())
        ("session-timeout-ms", "How long a client may stay silent (no proto Keepalive) before the server considers its session gone and removes it. Value in milliseconds, must be greater than 0. Default 5000ms. Should be several times the client keepalive interval (1000ms) so a single lost ping does not drop the session.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_TIMEOUT.count())))
        ("session-reap-interval-ms", "How often the server scans for sessions that have been silent longer than --session-timeout-ms. Value in milliseconds, must be greater than 0. Default 1000ms. It only controls how promptly an already-dead session is reclaimed, so keep it at or below --session-timeout-ms.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::SESSION_REAP_INTERVAL.count())))
        ("audio-queue-capacity", "Capacity (9..4096, default 16) of the buffer between audio capture and the network sender, measured in audio-packet slots. It absorbs capture/dispatch scheduling hiccups; it adds no steady-state latency because the queue stays near empty, and only delays audio if it actually fills up. Raise it if the capture thread and the network worker are scheduled on the same core. The lower bound (9) must exceed the pacing catch-up depth (8) so the queue cannot overflow before catch-up can drain it.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::DEFAULT_AUDIO_QUEUE_CAPACITY_SLOTS)))
        ("log-level", "Verbosity of log output; allowed values: trace|debug|info|warn|error|fatal.",
            cxxopts::value<std::string>()->default_value(aqua::log_level_name(aqua::default_log_level())))
        ("list-devices", "List available INPUT and OUTPUT audio devices with their IDs and default formats, then exit.",
            cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print this help text and exit.")
        ("version", "Print the server version and exit.",
            cxxopts::value<bool>()->default_value("false"));
    // clang-format on

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            std::cout << options.help() << '\n';
            return ParseOutcome::Help;
        }

        if (result["version"].as<bool>()) {
            std::cout << "aqua_server " AQUA_SERVER_CLI_VERSION << '\n';
            return ParseOutcome::Version;
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

        const bool has_encoding = result.count("audio-encoding") != 0;
        const bool has_channels = result.count("audio-channels") != 0;
        const bool has_sample_rate = result.count("audio-sample-rate") != 0;
        const bool any_format_option = has_encoding || has_channels || has_sample_rate;
        if (any_format_option && !(has_encoding && has_channels && has_sample_rate)) {
            std::cerr << "--audio-encoding, --audio-channels and --audio-sample-rate must be specified together; omit all three for backend default\n";
            return ParseOutcome::Error;
        }
        if (any_format_option) {
            const auto enc = parse_encoding(result["audio-encoding"].as<std::string>());
            if (!enc) {
                std::cerr << "invalid --audio-encoding\n";
                return ParseOutcome::Error;
            }
            const auto format = make_format(*enc,
                result["audio-channels"].as<std::uint32_t>(),
                result["audio-sample-rate"].as<std::uint32_t>());
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

        const auto requested_packet_frames = result["audio-packet-frames"].as<std::uint32_t>();
        if (requested_packet_frames != 0 && requested_packet_frames < kMinPacketFrames) {
            std::cerr << "invalid --audio-packet-frames: must be 0 (auto) or at least "
                      << kMinPacketFrames << "\n";
            return ParseOutcome::Error;
        }
        // 快错：显式 F 且格式已知时，直接按 MTU 预算拒绝（与 ServerRuntime::
        // resolve_effective_frame_count 同口径）。格式走后端默认时此处无法核算，
        // 留给启动期校验。
        if (requested_packet_frames != 0 && config.format.has_value()) {
            const auto payload_bytes = config.format->bytes_for_frames(requested_packet_frames);
            if (payload_bytes == 0 || payload_bytes > kMtuPayloadBudget) {
                std::cerr << "invalid --audio-packet-frames: frame payload exceeds UDP budget "
                          << kMtuPayloadBudget << " bytes for the requested format\n";
                return ParseOutcome::Error;
            }
        }

        const auto queue_capacity_slots = result["audio-queue-capacity"].as<std::uint32_t>();
        if (queue_capacity_slots < kMinAudioQueueCapacitySlots
            || queue_capacity_slots > kMaxAudioQueueCapacitySlots) {
            std::cerr << "invalid --audio-queue-capacity: expected "
                      << kMinAudioQueueCapacitySlots << ".." << kMaxAudioQueueCapacitySlots
                      << " (must exceed the pacing catch-up depth, or the queue drops frames "
                         "before catch-up can ever fire)\n";
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
        if (result["session-reap-interval-ms"].as<std::uint32_t>() == 0) {
            std::cerr << "invalid --session-reap-interval-ms: must be > 0\n";
            return ParseOutcome::Error;
        }

        config.frame_count = requested_packet_frames;
        config.server_ip = server_ip;
        config.udp_port = result["udp-port"].as<std::uint16_t>();
        if (config.udp_port == 0) {
            std::cerr << "invalid --udp-port: must be > 0\n";
            return ParseOutcome::Error;
        }
        config.session_timeout = std::chrono::milliseconds(result["session-timeout-ms"].as<std::uint32_t>());
        config.session_reap_interval = std::chrono::milliseconds(result["session-reap-interval-ms"].as<std::uint32_t>());
        config.audio_queue_capacity_slots = queue_capacity_slots;
        config.capture.source = (capture_mode == "input")
            ? audio::AudioCaptureSource::INPUT_DEVICE
            : audio::AudioCaptureSource::OUTPUT_LOOPBACK;
        if (result.count("capture-device-id") != 0) {
            const auto id = result["capture-device-id"].as<std::string>();
            if (id.empty()) {
                std::cerr << "invalid --capture-device-id: value must not be empty\n";
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
                    std::cerr << "invalid --capture-device-id: cannot resolve the specified " << expected
                              << " capture endpoint (device may not exist or has the wrong direction)\n";
                    return ParseOutcome::Error;
                }
            }
        } else {
            config.capture.device.reset();
        }
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        // 默认通告地址/端口跟随 server_ip / udp_port；显式 udp-udp-advertise 参数用于部署在
        // NAT、容器或多网卡环境时指定 client 实际可达的数据面 endpoint。
        if (result.count("udp-advertise-ip") != 0) {
            config.advertised_udp_address = result["udp-advertise-ip"].as<std::string>();
            // 通告地址是 client 实际拨号的目标：通配符（0.0.0.0/::）不可达，必须拒绝。
            // server-ip（本地绑定）允许通配，两者语义不同。
            if (!validate_ip_literal(config.advertised_udp_address, "--udp-advertise-ip", false)) {
                return ParseOutcome::Error;
            }
        } else {
            config.advertised_udp_address.clear();
        }
        if (result.count("udp-advertise-port") != 0) {
            const auto port = result["udp-advertise-port"].as<std::uint16_t>();
            if (port == 0) {
                std::cerr << "invalid --udp-advertise-port: must be > 0\n";
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
