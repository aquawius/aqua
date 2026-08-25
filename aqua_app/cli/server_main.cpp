// aqua_server_cli：数据面验证用 server（无 gRPC）。
// 采集（loopback / 麦克风）→ 打包成固定 F 的 AudioFrame → 编码 → UDP 发往固定目的端点。

#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/packetizer/audio_packetizer.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/udp/udp_packet.h"
#include "aqua/net/udp/udp_server.h"

#include "cli_common.h"

#include <asio.hpp>
#include <cxxopts.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

struct ServerContext {
    ServerContext(asio::io_context& ioc, const asio::ip::udp::endpoint& dest,
        std::uint32_t frames_per_slot, std::uint32_t frame_bytes)
        : dest(dest)
        , udp(ioc)
        , packetizer(frames_per_slot, frame_bytes)
    {
    }

    asio::ip::udp::endpoint dest;
    aqua::net::UdpServer udp;
    aqua::audio::AudioPacketizer packetizer;
    std::atomic<std::uint64_t> frames_sent { 0 };
};

void on_packetized(void* ud, std::uint64_t sequence, std::span<const std::byte> pcm) noexcept
{
    auto* ctx = static_cast<ServerContext*>(ud);
    auto packet = std::make_shared<const std::vector<std::byte>>(
        aqua::net::encode_audio_packet(sequence, pcm));
    ctx->udp.send_shared(ctx->dest, std::move(packet));
    ctx->frames_sent.fetch_add(1, std::memory_order_relaxed);
}

void on_capture(void* ud, const aqua::audio::AudioFrame& frame) noexcept
{
    static_cast<ServerContext*>(ud)->packetizer.push(frame.data, on_packetized, ud);
}

} // namespace

int main(int argc, char** argv)
{
    cxxopts::Options options("aqua_server", "Aqua audio server (validation, no gRPC)");
    options.add_options()
        ("udp-ip", "UDP destination IP", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("udp-port", "UDP destination port", cxxopts::value<std::uint16_t>()->default_value("9999"))
        ("encoding", "PCM encoding: s16|s24|s32|f32|u8", cxxopts::value<std::string>()->default_value("f32"))
        ("channels", "channel count", cxxopts::value<std::uint32_t>()->default_value("2"))
        ("sample-rate", "sample rate (Hz)", cxxopts::value<std::uint32_t>()->default_value("48000"))
        ("frames-per-slot", "frames per AudioFrame (0=auto from MTU)", cxxopts::value<std::uint32_t>()->default_value("0"))
        ("capture", "capture source: loopback|input", cxxopts::value<std::string>()->default_value("loopback"))
        ("device-id", "specific device id", cxxopts::value<std::string>())
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
    if (!aqua::audio::AudioPacketizer::is_valid_config(fps, format.frame_bytes())) {
        std::cerr << "invalid frames-per-slot config\n";
        return 1;
    }

    asio::error_code ec;
    const auto dest_addr = asio::ip::make_address(result["udp-ip"].as<std::string>(), ec);
    if (ec) {
        std::cerr << "invalid --udp-ip: " << ec.message() << '\n';
        return 1;
    }
    const asio::ip::udp::endpoint dest(dest_addr, result["udp-port"].as<std::uint16_t>());

    auto device_mgr = aqua::audio::create_device_manager();
    if (!device_mgr) {
        std::cerr << "no device manager for this platform\n";
        return 1;
    }
    auto capture = aqua::audio::create_capture(*device_mgr);
    if (!capture) {
        std::cerr << "no capture backend for this platform\n";
        return 1;
    }

    aqua::audio::AudioCaptureConfig cfg;
    cfg.source = (result["capture"].as<std::string>() == "input")
        ? aqua::audio::AudioCaptureSource::INPUT_DEVICE
        : aqua::audio::AudioCaptureSource::OUTPUT_LOOPBACK;
    if (result.count("device-id") != 0) {
        cfg.device = aqua::audio::AudioDeviceId(result["device-id"].as<std::string>());
    }
    cfg.format = format;

    asio::io_context ioc;
    ServerContext ctx(ioc, dest, fps, format.frame_bytes());

    if (!ctx.udp.bind("0.0.0.0", 0)) {
        std::cerr << "failed to bind UDP\n";
        return 1;
    }

    if (!capture->start(cfg, on_capture, &ctx)) {
        std::cerr << "failed to start capture\n";
        return 1;
    }

    aqua::log_info_fmt("server: capture started, sending to {}:{} ({}ch/{}Hz/{}, F={})",
        result["udp-ip"].as<std::string>(), result["udp-port"].as<std::uint16_t>(),
        format.channels, format.sample_rate, result["encoding"].as<std::string>(), fps);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("udp", [&ctx]() {
        const auto s = ctx.udp.stats();
        return std::format("tx_packets={} tx_bytes={} tx_dropped={} tx_queue={}",
            s.tx_packets, s.tx_bytes, s.tx_dropped, s.tx_queue_depth);
    });
    diag.add_source("packetizer", [&ctx]() {
        return std::format("frames_emitted={} frames_sent={}",
            ctx.packetizer.frames_emitted(), ctx.frames_sent.load());
    });
    diag.add_source("capture", [&capture]() {
        return std::format("running={}", capture->is_running());
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
        capture->stop();
        ctx.udp.stop();
        ioc.stop();
    });

    ioc.run();

    aqua::log_info_fmt("server: stopped, frames sent={}", ctx.frames_sent.load());
    return 0;
}
