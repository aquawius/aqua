// aqua_server_cli：完整 server（gRPC 控制面 + UDP 数据面）。
// 采集（loopback / 麦克风）→ ServerRuntime 打包成固定 F 的 AudioFrame → 广播给所有 Connected session。
// session 生命周期由 gRPC Connect/Disconnect + UDP HELLO 保活共同维护（见 ServerRuntime / GrpcServer）。

#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/grpc/grpc_server.h"
#include "aqua/runtime/server_runtime.h"

#include "cli_common.h"

#include <asio.hpp>
#include <cxxopts.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

int main(int argc, char** argv)
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

    asio::io_context ioc;

    aqua::runtime::ServerRuntimeConfig rt_cfg;
    rt_cfg.format = format;
    rt_cfg.frames_per_slot = fps;
    rt_cfg.udp_bind_ip = result["udp-ip"].as<std::string>();
    rt_cfg.udp_port = result["udp-port"].as<std::uint16_t>();
    rt_cfg.session_timeout = std::chrono::milliseconds(result["session-timeout-ms"].as<std::uint32_t>());
    rt_cfg.session_reap_interval = std::chrono::milliseconds(result["reap-interval-ms"].as<std::uint32_t>());

    // ServerRuntime 经 shared_from_this 绑定 reap 定时器，必须用 make_shared 创建。
    auto runtime = std::make_shared<aqua::runtime::ServerRuntime>(ioc, rt_cfg);
    if (!runtime->start()) {
        std::cerr << "failed to start server data plane\n";
        return 1;
    }

    const std::string rpc_ip = result["rpc-ip"].as<std::string>();
    const std::uint16_t rpc_port = result["rpc-port"].as<std::uint16_t>();
    const std::string advertise_ip = result["advertise-ip"].as<std::string>();

    aqua::grpc::GrpcServer grpc(runtime->sessions(), format, fps,
        rpc_ip, rpc_port, advertise_ip, result["udp-port"].as<std::uint16_t>());
    std::thread grpc_thread([&grpc] { grpc.run(); });
    // run() 在独立线程进入 Wait() 后才置 running_；轮询等待，超时视为启动失败。
    for (int i = 0; i < 100 && !grpc.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!grpc.is_running()) {
        std::cerr << "failed to start gRPC server on " << rpc_ip << ':' << rpc_port << '\n';
        grpc.shutdown();
        if (grpc_thread.joinable()) {
            grpc_thread.join();
        }
        runtime->stop();
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

    // 采集回调只捕获 runtime（shared_ptr）：即使控制线程先行退出，音频回调期间
    // runtime 仍保活，无 UAF；stop() 会 join 音频线程，保证回调不再发生。
    auto on_capture = [runtime](const aqua::audio::AudioBlock& block) noexcept {
        runtime->push_pcm(block.data);
    };
    if (!capture->start(cfg, std::move(on_capture), aqua::audio::AudioCaptureEventCallback {})) {
        std::cerr << "failed to start capture\n";
        grpc.shutdown();
        if (grpc_thread.joinable()) {
            grpc_thread.join();
        }
        runtime->stop();
        return 1;
    }

    aqua::log_info_fmt("server: gRPC {}:{} udp {}:{} ({}ch/{}Hz/{}, F={})",
        rpc_ip, rpc_port, advertise_ip, result["udp-port"].as<std::uint16_t>(),
        format.channels, format.sample_rate, result["encoding"].as<std::string>(), fps);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("udp", [runtime]() {
        return std::format("frames_broadcast={} sessions={}",
            runtime->frames_broadcast(), runtime->sessions().session_count());
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
        runtime->stop();
        grpc.shutdown();
        ioc.stop();
    });

    ioc.run();

    // 幂等清理：信号路径与正常退出路径共用同一套收尾。
    capture->stop();
    runtime->stop();
    grpc.shutdown();
    if (grpc_thread.joinable()) {
        grpc_thread.join();
    }

    aqua::log_info_fmt("server: stopped, frames_broadcast={}",
        runtime->frames_broadcast());
    return 0;
}
