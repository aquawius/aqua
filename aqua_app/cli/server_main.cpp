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
    switch (aqua::cli::parse_server_cli(argc, argv, cfg)) {
    case aqua::cli::ParseOutcome::Run:
        break;
    case aqua::cli::ParseOutcome::Help:
        return 0;
    case aqua::cli::ParseOutcome::Error:
        return 1;
    }

    aqua::init_logger();
    aqua::set_log_level(aqua::default_log_level());

    asio::io_context ioc;
    auto server = std::make_shared<aqua::runtime::ServerRuntime>(ioc, cfg);
    if (!server->start()) {
        std::cerr << "failed to start server\n";
        return 1;
    }

    aqua::log_info_fmt("server: gRPC {}:{} udp {}:{} ({}ch/{}Hz/enc={}, F={})",
        cfg.rpc_bind_ip, cfg.rpc_port, cfg.advertised_udp_address, cfg.udp_port,
        cfg.format.channels, cfg.format.sample_rate,
        static_cast<int>(cfg.format.encoding), cfg.frame_count);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("udp", [&server]() {
        return std::format("encoded={} broadcast={} no_clients={} dropped={} sessions={}",
            server->frames_encoded(), server->frames_broadcast(),
            server->frames_without_clients(), server->frames_dropped_before_network(),
            server->sessions().session_count());
    });
    diag.add_source("runtime", [&server]() {
        return std::format("state={} audio_error={}",
            aqua::runtime::runtime_state_name(server->state()),
            aqua::audio::audio_error_name(server->last_audio_error()));
    });
    diag.add_source("capture", [&server]() {
        return std::format("running={}", server->capture_running());
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
        server->stop();
        ioc.stop();
    });

    ioc.run();

    server->stop();

    aqua::log_info_fmt("server: stopped, state={}, frames_encoded={}",
        aqua::runtime::runtime_state_name(server->state()), server->frames_encoded());
    return 0;
}
