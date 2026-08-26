// aqua_client_cli：数据面验证用 client（无 gRPC）。
// UDP 收 Audio datagram → 解码 → JitterBuffer → 回放后端 pull。

#include "aqua/audio/audio_frame.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/udp/network_frame.h"
#include "aqua/net/udp/udp_server.h"

#include "cli_common.h"

#include <asio.hpp>
#include <cxxopts.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>

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
        std::cerr << "invalid --frames-per-slot: must be > 0 and fit within MTU budget\n";
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
            const auto nf = aqua::net::NetworkFrame::decode(data);
            if (!nf || nf->type() != aqua::net::PacketType::Audio) {
                return;
            }
            aqua::audio::AudioFrame frame { nf->sequence(), fps, nf->payload() };
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

    if (!playback->start(pb_cfg, [jb](std::span<std::byte> output) noexcept {
            return jb->pull(output).frames_filled;
        })) {
        std::cerr << "failed to start playback\n";
        return 1;
    }

    aqua::log_info_fmt("client: listening on {}:{} ({}ch/{}Hz/{}, F={})",
        result["udp-ip"].as<std::string>(), result["udp-port"].as<std::uint16_t>(),
        format.channels, format.sample_rate, result["encoding"].as<std::string>(), fps);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("udp", [&udp]() {
        const auto s = udp.stats();
        return std::format("rx_packets={} rx_bytes={} rx_errors={}",
            s.rx_packets, s.rx_bytes, s.rx_errors);
    });
    diag.add_source("jitter", [&jb]() {
        return std::format("water={:.2f} used={}/{}",
            jb->water_level(), jb->used_slots(), jb->capacity_slots());
    });
    diag.add_source("playback", [&playback]() {
        return std::format("running={}", playback->is_running());
    });

    auto diag_timer = std::make_shared<asio::steady_timer>(ioc);
    std::function<void(const asio::error_code&)> diag_tick;
    diag_tick = [diag_timer, &diag, &diag_tick](const asio::error_code& ec) {
        if (ec) {
            return;
        }
        diag.print();
        diag_timer->expires_after(std::chrono::seconds(1));
        diag_timer->async_wait(diag_tick);
    };
    diag_tick(asio::error_code {});

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
