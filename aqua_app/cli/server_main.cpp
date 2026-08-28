// aqua_server_cli：完整 server。参数解析在 cli_parser_server，装配与生命周期在 ServerRuntime。

#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/runtime/server_runtime.h"

#include "cli_parser/cli_parser_server.h"

#include <asio.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    aqua::runtime::ServerRuntimeConfig cfg;
    aqua::LogLevel log_level = aqua::default_log_level();
    switch (aqua::cli::parse_server_cli(argc, argv, cfg, log_level)) {
    case aqua::cli::ParseOutcome::Run:
        break;
    case aqua::cli::ParseOutcome::Help:
        return 0;
    case aqua::cli::ParseOutcome::Error:
        return 1;
    }

    aqua::init_logger();
    aqua::set_log_level(log_level);
    aqua::log_debug_fmt("CLI config: log_level={} udp={}:{} rpc={}:{} advertise={} format={}ch/{}Hz/enc={} frame_count={} queue_slots={} capture_source={} capture_device={}",
        aqua::log_level_name(log_level), cfg.udp_bind_ip, cfg.udp_port, cfg.rpc_bind_ip, cfg.rpc_port,
        cfg.advertised_udp_address, cfg.format.channels, cfg.format.sample_rate,
        static_cast<int>(cfg.format.encoding), cfg.frame_count, cfg.network_queue_slots,
        static_cast<int>(cfg.capture.source), cfg.capture.device ? cfg.capture.device->value() : std::string("default"));

    asio::io_context ioc;
    auto server = std::make_shared<aqua::runtime::ServerRuntime>(ioc, cfg);
    if (!server->start()) {
        aqua::log_fatal("server failed to start; see preceding error for details");
        return 1;
    }

    aqua::log_info_fmt("server: gRPC {}:{} udp {}:{} ({}ch/{}Hz/enc={}, F={})",
        cfg.rpc_bind_ip, cfg.rpc_port, cfg.advertised_udp_address, server->udp_port(),
        cfg.format.channels, cfg.format.sample_rate,
        static_cast<int>(cfg.format.encoding), cfg.frame_count);

    aqua::diagnostics::Diagnostics diag("Server");
    diag.add_source("state", [&server]() {
        return std::format("state={} sessions={}",
            aqua::runtime::runtime_state_name(server->state()),
            server->session_count());
    });
    diag.add_source("audio", [&server]() {
        return std::format("capture={} error={} packetizer_unaligned={}",
            server->capture_running(),
            aqua::audio::audio_error_name(server->last_audio_error()),
            server->packetizer_rejected_unaligned_blocks());
    });
    diag.add_source("frames", [&server]() {
        return std::format("encoded={} broadcast={} no_clients={} encode_fail={} dispatch_fail={} queue_drop={}",
            server->frames_encoded(), server->frames_broadcast(), server->frames_without_clients(),
            server->encode_failures(), server->dispatch_failures(),
            server->frames_dropped_before_network());
    });
    diag.add_source("udp", [&server]() {
        const auto s = server->udp_stats();
        return std::format("rx={} rxB={} rxerr={} tx={} txB={} txerr={} drop={} enqfail={} q={}",
            s.rx_packets, s.rx_bytes, s.rx_errors, s.tx_packets, s.tx_bytes,
            s.tx_errors, s.tx_dropped, s.tx_enqueue_failures, s.tx_queue_depth);
    });

    auto diag_timer = std::make_shared<asio::steady_timer>(ioc);
    std::function<void(const asio::error_code&)> diag_tick;
    diag_tick = [diag_timer, &diag, &diag_tick](const asio::error_code& ec) {
        if (ec) {
            return;
        }
        diag.log_debug();
        diag_timer->expires_after(std::chrono::seconds(1));
        diag_timer->async_wait(diag_tick);
    };
    diag_tick(asio::error_code {});
    aqua::log_debug("server: diagnostics snapshot interval=1000ms");

    auto control_timer = std::make_shared<asio::steady_timer>(ioc);
    std::function<void(const asio::error_code&)> control_tick;
    control_tick = [control_timer, &control_tick, server, &ioc](const asio::error_code& ec) {
        if (ec) {
            return;
        }
        if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
            aqua::log_trace_fmt("server: control poll tick state={}",
                aqua::runtime::runtime_state_name(server->state()));
        }
        if (server->state() == aqua::runtime::RuntimeState::Degraded) {
            aqua::log_debug_fmt("server: control poll observed terminal condition: state={}",
                aqua::runtime::runtime_state_name(server->state()));
            server->stop();
            ioc.stop();
            return;
        }
        control_timer->expires_after(aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL);
        control_timer->async_wait(control_tick);
    };
    control_tick(asio::error_code {});

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code& ec, int signal_number) {
        if (!ec) {
            aqua::log_info_fmt("server: shutdown requested by signal {}", signal_number);
        }
        server->stop();
        ioc.stop();
    });

    ioc.run();

    server->stop();

    aqua::log_info_fmt("server: stopped, frames_encoded={}", server->frames_encoded());
    return 0;
}
