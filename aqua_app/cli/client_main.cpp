// aqua_client_cli：数据面验证用 client（无 gRPC）。
// UDP 收 Audio datagram → 解码 → JitterBuffer → 回放后端 pull。

#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/logger/logger.h"
#include "aqua/net/udp/udp_packet.h"
#include "aqua/net/udp/udp_server.h"

#include "cli_common.h"

#include <asio.hpp>
#include <cxxopts.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>

namespace {

std::uint32_t on_playback(void* ud, std::span<std::byte> output) noexcept
{
    return static_cast<aqua::audio::JitterBuffer*>(ud)->pull(output).frames_filled;
}

} // namespace

int main(int argc, char** argv)
{
    cxxopts::Options options("aqua_client", "Aqua audio client (validation, no gRPC)");
    options.add_options()
        ("udp-ip", "UDP bind IP", cxxopts::value<std::string>()->default_value("0.0.0.0"))
        ("udp-port", "UDP bind port", cxxopts::value<std::uint16_t>()->default_value("9999"))
        ("encoding", "PCM encoding: s16|s24|s32|f32|u8", cxxopts::value<std::string>()->default_value("f32"))
        ("channels", "channel count", cxxopts::value<std::uint32_t>()->default_value("2"))
        ("sample-rate", "sample rate (Hz)", cxxopts::value<std::uint32_t>()->default_value("48000"))
        ("frames-per-slot", "frames per AudioFrame (0=auto from MTU)", cxxopts::value<std::uint32_t>()->default_value("0"))
        ("jitter-slots", "jitter buffer slot count", cxxopts::value<std::uint32_t>()->default_value("30"))
        ("device-id", "playback device id", cxxopts::value<std::string>())
        ("h,help", "print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help") != 0) {
        std::cout << options.help() << '\n';
        return 0;
    }

    aqua::init_logger();
    aqua::set_log_level(aqua::default_log_level());

    const auto enc = aqua::cli::parse_encoding(result["encoding"].as<std::string>());
    if (!enc) {
        std::cerr << "invalid --encoding\n";
        return 1;
    }
    const auto format = aqua::cli::make_format(*enc,
        result["channels"].as<std::uint32_t>(),
        result["sample-rate"].as<std::uint32_t>());
    if (!format.is_valid()) {
        std::cerr << "invalid audio format\n";
        return 1;
    }
    const auto fps = aqua::cli::resolve_frames_per_slot(
        result["frames-per-slot"].as<std::uint32_t>(), format);
    if (fps == 0) {
        std::cerr << "cannot derive frames-per-slot\n";
        return 1;
    }

    // JitterBuffer
    aqua::audio::JitterBufferConfig jb_cfg;
    jb_cfg.capacity_slots = result["jitter-slots"].as<std::uint32_t>();
    jb_cfg.format = format;
    jb_cfg.frames_per_slot = fps;
    auto jb_res = aqua::audio::JitterBuffer::create(jb_cfg);
    if (!jb_res) {
        std::cerr << "failed to create jitter buffer\n";
        return 1;
    }
    auto jb = std::shared_ptr<aqua::audio::JitterBuffer>(std::move(*jb_res));

    asio::error_code ec;
    (void)asio::ip::make_address(result["udp-ip"].as<std::string>(), ec);
    if (ec) {
        std::cerr << "invalid --udp-ip: " << ec.message() << '\n';
        return 1;
    }

    asio::io_context ioc;
    aqua::net::UdpServer udp(ioc);
    if (!udp.bind(result["udp-ip"].as<std::string>(), result["udp-port"].as<std::uint16_t>())) {
        std::cerr << "failed to bind UDP\n";
        return 1;
    }

    // 收包：Audio → 解码 → JB。捕获 shared_ptr<JB> 保证生命周期。
    if (!udp.start_receive([jb, fps](const asio::ip::udp::endpoint&,
                               std::span<const std::byte> data) {
            if (aqua::net::decode_packet_type(data) != aqua::net::PacketType::Audio) {
                return;
            }
            std::uint64_t sequence = 0;
            std::span<const std::byte> pcm;
            if (!aqua::net::decode_audio_packet(data, sequence, pcm)) {
                return;
            }
            aqua::audio::AudioFrame frame { sequence, 0, fps, pcm };
            jb->push(frame);
        })) {
        std::cerr << "failed to start receive\n";
        return 1;
    }

    auto device_mgr = aqua::audio::create_device_manager();
    if (!device_mgr) {
        std::cerr << "no device manager for this platform\n";
        return 1;
    }
    auto playback = aqua::audio::create_playback(*device_mgr);
    if (!playback) {
        std::cerr << "no playback backend for this platform\n";
        return 1;
    }

    aqua::audio::AudioPlaybackConfig pb_cfg;
    if (result.count("device-id") != 0) {
        pb_cfg.device = aqua::audio::AudioDeviceId(result["device-id"].as<std::string>());
    }
    pb_cfg.format = format;

    if (!playback->start(pb_cfg, on_playback, jb.get())) {
        std::cerr << "failed to start playback\n";
        return 1;
    }

    aqua::log_info_fmt("client: listening on {}:{} ({}ch/{}Hz/{}, F={})",
        result["udp-ip"].as<std::string>(), result["udp-port"].as<std::uint16_t>(),
        format.channels, format.sample_rate, result["encoding"].as<std::string>(), fps);

    asio::signal_set signals(ioc, SIGINT);
    signals.async_wait([&](const asio::error_code&, int) {
        playback->stop();
        udp.stop();
        ioc.stop();
    });

    ioc.run();

    aqua::log_info("client: stopped");
    return 0;
}
