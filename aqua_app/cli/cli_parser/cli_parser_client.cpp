#include "cli_parser_client.h"

#include <aqua_app/cli/cli_version.h>

#include "aqua/net/address/address_utils.h"
#include "aqua/net/grpc/grpc_config.h"

#include <cxxopts.hpp>

#include <cstdint>
#include <format>
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
    // clang-format off: cxxopts 选项链刻意一选项一行；formatter 的 BinPack 输出不可读。
    options.add_options()
        ("server-ip", "IP address of the server to connect to (its gRPC control-plane address). Required. Must be a concrete IP literal (IPv4 or IPv6) - DNS host names are not resolved and wildcard addresses are rejected.",
            cxxopts::value<std::string>())
        ("rpc-port", "TCP port (1..65535) of the server's gRPC control plane, which carries Connect / Disconnect / Keepalive. Default 50051. The UDP audio port is learned from the server over gRPC, not from this option.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultRpcPort)))
        ("udp-force-port", "UDP port (1..65535) to use instead of the one the server advertised; useful when a NAT or port-forward requires a different port. Defaults to the server-advertised port.",
            cxxopts::value<std::uint16_t>())
        ("client-name", "Client name sent to the server, used only for identification and logging (1..128 bytes). Default 'aqua-client'.",
            cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_CLIENT_NAME))
        ("jb-capacity", "Playback jitter buffer size in slots (4..4096, default 30). One slot holds one UDP audio packet - 3.75ms at 180 frames/48kHz - so 30 slots is about 112ms of buffer. Bigger tolerates more network jitter but adds playback latency; this is the main latency/stability dial. The adaptive target is capped at 2/3 of this value, so capacity beyond that buys jitter headroom rather than delay. 4 is a hard structural floor: below it the five water-level bands can no longer be strictly ordered and the buffer refuses to start.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::JB_DEFAULT_CAPACITY_SLOTS)))
        ("jb-jitter-gain", "Adaptive target gain k (default 5.0; only used when adaptive jitter is on). target = clamp(base + k*J, floor, 2/3*capacity) in packets, where J is the RFC 3550 mean interarrival jitter. J is a mean while target must cover the peak, and Aqua's server sends in bursts, so k around 5 is needed on a 180-frame/48kHz link; each +1 adds roughly J/packet_ms slots. Very large values do not fail - they are clamped at the 2/3 structural cap, so the target simply pins there. 0 lets a clean link fall all the way to the floor.",
            cxxopts::value<double>()->default_value(std::format("{:g}", aqua::config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN)))
        ("jb-min-target", "Hard lower bound in slots for the adaptive target (default 3). The effective floor is max(this, geometric floor), where the geometric floor is one playback callback's packets + 1 - it always wins, so this option can only RAISE the floor and can never push the target below it. Raise it to buy a higher minimum latency floor on a link that keeps underrunning; going lower than the geometric floor is not possible through the CLI.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS)))
        ("jb-fixed-target", "Disable the adaptive jitter target: use the legacy fixed target and startup water levels (0.60 / 0.50 of capacity) instead of adapting to measured arrival jitter. Default is adaptive. Useful as an A/B baseline when tuning.",
            cxxopts::value<bool>()->default_value("false"))
        ("jb-no-conceal", "Disable PCM concealment: play silence for missing packets instead of repeating the last valid packet with a short fade-out (up to 3 packets, then silence). Default is concealment on. Turn it off if you would rather hear dropouts than smeared audio.",
            cxxopts::value<bool>()->default_value("false"))
        ("playback-device-id", "Playback OUTPUT device ID to use instead of the system default; list available IDs with --list-devices. Ignored if the device cannot be resolved as an OUTPUT endpoint.",
            cxxopts::value<std::string>())
        ("log-level", "Verbosity of log output; allowed values: trace|debug|info|warn|error|fatal. 'debug' additionally prints a one-line diagnostics snapshot once per second.",
            cxxopts::value<std::string>()->default_value(aqua::log_level_name(aqua::default_log_level())))
        ("list-devices", "List available OUTPUT playback devices with their IDs and default formats, then exit.",
            cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print this help text and exit.")
        ("version", "Print the client version and exit.",
            cxxopts::value<bool>()->default_value("false"));
    // clang-format on

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help") != 0) {
            std::cout << options.help() << '\n';
            return ParseOutcome::Help;
        }

        if (result["version"].as<bool>()) {
            std::cout << "aqua_client " AQUA_CLIENT_CLI_VERSION << '\n';
            return ParseOutcome::Version;
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
        config.jb_capacity_slots = result["jb-capacity"].as<std::uint32_t>();
        config.jb_adaptive_target = !result["jb-fixed-target"].as<bool>();
        config.jb_pcm_concealment = !result["jb-no-conceal"].as<bool>();
        config.jb_jitter_gain = result["jb-jitter-gain"].as<double>();
        config.jb_min_target_slots = result["jb-min-target"].as<std::uint32_t>();
        config.server_ip = result["server-ip"].as<std::string>();
        config.rpc_port = result["rpc-port"].as<std::uint16_t>();
        config.client_name = result["client-name"].as<std::string>();

        // --jb-capacity 的下限是结构性的（低于 4 时五个水位带无法保持严格序），
        // 上限只是护栏。
        // gain / min-target 不做区间限制：gain 超出的部分由 2/3 结构上限接住，
        // NaN / Inf / 负值由 TargetController 退回默认；min-target 低于几何地板
        // 时被地板无条件托底，高于 capacity 时被 floor_target() 夹回容量。
        if (config.jb_capacity_slots < aqua::config::MIN_JB_CAPACITY_SLOTS
            || config.jb_capacity_slots > kMaxJbCapacitySlots) {
            std::cerr << "invalid --jb-capacity: expected " << aqua::config::MIN_JB_CAPACITY_SLOTS
                      << ".." << kMaxJbCapacitySlots
                      << " (below " << aqua::config::MIN_JB_CAPACITY_SLOTS
                      << " the five water-level bands cannot stay strictly ordered)\n";
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
            std::cerr << "invalid --server-ip: " << format_exception_message(e) << "\n";
            return ParseOutcome::Error;
        }
        if (config.rpc_port == 0) {
            std::cerr << "invalid --rpc-port: must be > 0\n";
            return ParseOutcome::Error;
        }
        if (result.count("udp-force-port") != 0) {
            const auto port = result["udp-force-port"].as<std::uint16_t>();
            if (port == 0) {
                std::cerr << "invalid --udp-force-port: must be > 0\n";
                return ParseOutcome::Error;
            }
            config.udp_force_port = port;
        } else {
            config.udp_force_port.reset();
        }
        if (config.client_name.empty() || config.client_name.size() > aqua::config::GRPC_MAX_CLIENT_NAME_BYTES) {
            std::cerr << "invalid --client-name: expected 1.." << aqua::config::GRPC_MAX_CLIENT_NAME_BYTES << " bytes\n";
            return ParseOutcome::Error;
        }
        const auto parsed_log_level = aqua::string_to_log_level_enum(result["log-level"].as<std::string>());
        if (!parsed_log_level) {
            std::cerr << "invalid --log-level: expected trace|debug|info|warn|error|fatal\n";
            return ParseOutcome::Error;
        }
        log_level = *parsed_log_level;
        if (result.count("playback-device-id") != 0) {
            const auto id = result["playback-device-id"].as<std::string>();
            if (id.empty()) {
                std::cerr << "invalid --playback-device-id: value must not be empty\n";
                return ParseOutcome::Error;
            }
            config.playback.device = audio::AudioDeviceId(id);
            if (auto manager = audio::create_device_manager()) {
                const auto resolved = manager->resolve(audio::AudioDeviceDirection::OUTPUT, config.playback.device);
                if (!resolved) {
                    std::cerr << "invalid --playback-device-id: cannot resolve the specified OUTPUT playback endpoint "
                              << "(device may not exist or is not an OUTPUT endpoint)\n";
                    return ParseOutcome::Error;
                }
            }
        } else {
            config.playback.device.reset();
        }

        return ParseOutcome::Run;
    } catch (const std::exception& e) {
        std::cerr << format_exception_message(e) << '\n';
        return ParseOutcome::Error;
    }
}

} // namespace aqua::cli
