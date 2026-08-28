// aqua_client_cli：完整 client。参数解析在 cli_parser_client，装配与生命周期在 ClientRuntime。

#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/runtime/client_runtime.h"

#include "cli_parser/cli_parser_client.h"

#include <asio.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <iostream>

int main(int argc, char** argv)
{
    aqua::runtime::ClientRuntimeConfig cfg;
    switch (aqua::cli::parse_client_cli(argc, argv, cfg)) {
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
    aqua::runtime::ClientRuntime client(ioc, cfg);
    if (!client.start()) {
        std::cerr << "failed to start client\n";
        return 1;
    }

    const auto& cr = client.connect_result();
    aqua::log_info_fmt("client: session=0x{:08X} server={}:{} ({}ch/{}Hz/enc={}, F={})",
        cr.session_id, cr.udp_address, cr.udp_port,
        cr.audio_format.channels, cr.audio_format.sample_rate,
        static_cast<int>(cr.audio_format.encoding), cr.frame_count);

    aqua::diagnostics::Diagnostics diag;
    diag.add_source("jitter", [&client]() {
        return std::format("water={:.2f} used={}/{} reanchor={} sanity_reject={} last_reanchor={}",
            client.jitter_water_level(), client.jitter_used_slots(), client.jitter_capacity_slots(),
            client.jitter_reanchor_count(), client.jitter_reanchor_sanity_rejections(),
            client.jitter_last_reanchor_sequence());
    });
    diag.add_source("network", [&client]() {
        return std::format("hello_ack={} misses={} age_ms={} udp_enqueue_fail={}",
            client.hello_ack_count(), client.hello_ack_misses(), client.hello_ack_age_ms(),
            client.udp_tx_enqueue_failures());
    });
    diag.add_source("playback", [&client]() {
        return std::format("state={} running={} audio_error={}",
            aqua::runtime::runtime_state_name(client.state()),
            client.playback_running(),
            aqua::audio::audio_error_name(client.last_audio_error()));
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

    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const asio::error_code&, int) {
        client.stop();
        ioc.stop();
    });

    ioc.run();

    client.stop();

    aqua::log_info("client: stopped");
    return 0;
}
