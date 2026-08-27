// aqua_client_cli：完整 client（gRPC 控制面 + UDP 数据面）。
// gRPC Connect 拿 session/format → ClientRuntime 收 Audio 帧进 JitterBuffer → 回放后端 pull。

#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/runtime/client_runtime.h"

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
    cxxopts::Options options("aqua_client", "Aqua audio client (gRPC control + UDP data plane)");
    options.add_options()
        ("server-ip", "server IP", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("rpc-port", "server gRPC port", cxxopts::value<std::uint16_t>()->default_value("50051"))
        ("name", "client name", cxxopts::value<std::string>()->default_value("aqua-client"))
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

    asio::io_context ioc;

    aqua::runtime::ClientRuntimeConfig rt_cfg;
    rt_cfg.jitter_buffer_slots = result["jitter-slots"].as<std::uint32_t>();

    aqua::runtime::ClientRuntime runtime(ioc, rt_cfg);
    if (!runtime.connect(result["server-ip"].as<std::string>(),
            result["rpc-port"].as<std::uint16_t>(),
            result["name"].as<std::string>())) {
        std::cerr << "failed to connect to server\n";
        return 1;
    }
    if (!runtime.start()) {
        std::cerr << "failed to start client data plane\n";
        return 1;
    }

    const auto& cr = runtime.connect_result();

    aqua::audio::AudioPlaybackConfig pb_cfg;
    if (result.count("device-id") != 0) {
        pb_cfg.device = aqua::audio::AudioDeviceId(result["device-id"].as<std::string>());
    }
    pb_cfg.format = cr.audio_format; // server 权威格式，客户端不做转换

    if (!playback->start(pb_cfg, [&runtime](std::span<std::byte> output) noexcept {
            return runtime.pull_playback(output);
        })) {
        std::cerr << "failed to start playback\n";
        runtime.stop();
        return 1;
    }

    aqua::log_info_fmt("client: session=0x{:08X} server={}:{} ({}ch/{}Hz/enc={}, F={})",
        cr.session_id, cr.udp_address, cr.udp_port,
        cr.audio_format.channels, cr.audio_format.sample_rate,
        static_cast<int>(cr.audio_format.encoding), cr.frame_count);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("jitter", [&runtime]() {
        auto* jb = runtime.jitter_buffer();
        if (jb == nullptr) {
            return std::string("n/a");
        }
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
        runtime.stop();
        ioc.stop();
    });

    ioc.run();

    playback->stop();
    runtime.stop();

    aqua::log_info("client: stopped");
    return 0;
}
