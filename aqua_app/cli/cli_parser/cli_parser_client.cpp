#include "cli_parser_client.h"

#include <aqua_app/cli/cli_version.h>

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
    // clang-format off: cxxopts 选项链刻意一选项一行；formatter 的 BinPack 输出不可读。
    options.add_options()
        ("server-ip", "IP address of the server to connect to (its gRPC control-plane address). Required.",
            cxxopts::value<std::string>())
        ("server-rpc", "TCP port (1..65535) of the server's gRPC control plane.",
            cxxopts::value<std::uint16_t>()->default_value(std::to_string(kDefaultRpcPort)))
        ("force-udp-port", "UDP port (1..65535) to use instead of the one the server advertised; useful when a NAT or port-forward requires a different port. Defaults to the server-advertised port.",
            cxxopts::value<std::uint16_t>())
        ("name", "Client name sent to the server, used only for identification (1..128 bytes).",
            cxxopts::value<std::string>()->default_value(aqua::config::DEFAULT_CLIENT_NAME))
        ("jitter-slots", "Number of slots (4..4096) in the playback jitter buffer. Larger values tolerate more network jitter but add playback latency.",
            cxxopts::value<std::uint32_t>()->default_value(std::to_string(aqua::config::DEFAULT_CLIENT_JITTER_BUFFER_SLOTS)))
        ("fixed-jitter-target", "Disable adaptive jitter target: use the legacy fixed target/startup water levels instead of adapting to arrival jitter. Default is adaptive.",
            cxxopts::value<bool>()->default_value("false"))
        ("no-pcm-concealment", "Disable PCM concealment: play silence for missing packets instead of repeating the last valid packet with a short fade-out (max 3 packets before falling back to silence). Default is concealment on.",
            cxxopts::value<bool>()->default_value("false"))
        ("jb-jitter-gain", "Adaptive target gain k: target = clamp(base + k*J, floor, capacity) in packets. J is the RFC 3550 mean interarrival jitter, so k must cover the peak, not the mean; Aqua's server sends in bursts, which needs k around 5. Each +1 adds about J/packet_ms slots (about 1.2 slots on a 180-frame/48kHz link). Main latency<->stability dial. Only used when adaptive jitter is on.",
            cxxopts::value<double>()->default_value("5.0"))
        ("jb-min-target", "Hard lower bound (slots) for the adaptive target. The effective floor is max(this, one playback callback's packets + 1). Default 3 is usually shadowed by the geometric floor; raise it only to force a higher minimum.",
            cxxopts::value<std::uint32_t>()->default_value("3"))
        ("jb-initial-target", "Initial adaptive target (slots) before arrival jitter is measured. Startup pre-roll is max(3, this). 0 = use the geometric floor (recommended).",
            cxxopts::value<std::uint32_t>()->default_value("0"))
        ("jb-fall-rate", "How fast the adaptive target may fall back (slots/second) after the network recovers. Rising is always immediate; only falling is rate limited.",
            cxxopts::value<double>()->default_value("1.0"))
        ("jb-rise-dwell", "Peak-hold window (ms): after any target rise, falls are blocked for this long. Prevents the target from chattering when jitter oscillates across a slot boundary (e.g. a laptop on battery batching its sends). 0 disables.",
            cxxopts::value<double>()->default_value("3000"))
        ("jb-underrun-penalty", "Slots added to the target floor per underrun event: the closed-loop safety net, because the k*J term is a mean and cannot cover random jitter tails or loss. Decays back once underruns stop. 0 disables the feedback loop.",
            cxxopts::value<double>()->default_value("1.0"))
        ("jb-underrun-penalty-max", "Cap (slots) on the accumulated underrun penalty, so a pathological stretch cannot push the target to the whole buffer.",
            cxxopts::value<std::uint32_t>()->default_value("6"))
        ("jb-underrun-decay", "How fast the underrun penalty decays (slots/second) once underruns stop. Slower = stay safe longer after a bad patch.",
            cxxopts::value<double>()->default_value("0.5"))
        ("jb-conceal-max", "Maximum consecutive packets to conceal (repeat last valid PCM with a fade) before falling back to silence. 0 disables concealment.",
            cxxopts::value<std::uint32_t>()->default_value("3"))
        ("jb-stall-threshold", "Arrival gaps longer than this many packet periods count as a stall (outage) and are excluded from the jitter mean J, so a transient network stall does not blow the target up for many seconds. 0 disables stall detection (every gap feeds J, raw RFC 3550).",
            cxxopts::value<double>()->default_value("5.0"))
        ("device-id", "Playback OUTPUT device ID to use instead of the system default; list available IDs with --list-devices.",
            cxxopts::value<std::string>())
        ("log-level", "Verbosity of log output; allowed values: trace|debug|info|warn|error|fatal.",
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
        config.jitter_buffer_slots = result["jitter-slots"].as<std::uint32_t>();
        config.adaptive_jitter = !result["fixed-jitter-target"].as<bool>();
        config.pcm_concealment = !result["no-pcm-concealment"].as<bool>();
        config.jitter_gain = result["jb-jitter-gain"].as<double>();
        config.min_target_slots = result["jb-min-target"].as<std::uint32_t>();
        config.initial_target_slots = result["jb-initial-target"].as<std::uint32_t>();
        config.fall_rate_slots_per_sec = result["jb-fall-rate"].as<double>();
        config.rise_dwell_ms = result["jb-rise-dwell"].as<double>();
        config.underrun_penalty_slots = result["jb-underrun-penalty"].as<double>();
        config.underrun_penalty_max_slots = result["jb-underrun-penalty-max"].as<std::uint32_t>();
        config.underrun_penalty_decay_slots_per_sec = result["jb-underrun-decay"].as<double>();
        config.concealment_max_slots = result["jb-conceal-max"].as<std::uint32_t>();
        config.stall_threshold_packets = result["jb-stall-threshold"].as<double>();
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
            std::cerr << "invalid --server-ip: " << format_exception_message(e) << "\n";
            return ParseOutcome::Error;
        }
        if (config.rpc_port == 0) {
            std::cerr << "invalid --server-rpc: must be > 0\n";
            return ParseOutcome::Error;
        }
        if (result.count("force-udp-port") != 0) {
            const auto port = result["force-udp-port"].as<std::uint16_t>();
            if (port == 0) {
                std::cerr << "invalid --force-udp-port: must be > 0\n";
                return ParseOutcome::Error;
            }
            config.force_udp_port = port;
        } else {
            config.force_udp_port.reset();
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
            if (auto manager = audio::create_device_manager()) {
                const auto resolved = manager->resolve(audio::AudioDeviceDirection::OUTPUT, config.playback.device);
                if (!resolved) {
                    std::cerr << "invalid --device-id: cannot resolve the specified OUTPUT playback endpoint "
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
