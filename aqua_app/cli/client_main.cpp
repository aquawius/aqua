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
    aqua::cli::configure_console_utf8();
    aqua::runtime::ClientRuntimeConfig cfg;
    aqua::LogLevel log_level = aqua::default_log_level();
    if (const auto exit_code = aqua::cli::cli_exit_code(
            aqua::cli::parse_client_cli(argc, argv, cfg, log_level))) {
        return *exit_code;
    }

    try {
        aqua::init_logger();
        aqua::set_log_level(log_level);
        aqua::log_debug_fmt("CLI config: log_level={} server={}:{} client_name='{}' jitter_slots={} hello_interval={}ms playback_device={} playback_buffer_frames={}",
            aqua::log_level_name(log_level), cfg.server_ip, cfg.rpc_port, cfg.client_name,
            cfg.jitter_buffer_slots, cfg.hello_interval.count(),
            cfg.playback.device ? cfg.playback.device->value() : std::string("default"),
            cfg.playback.frames_per_buffer);

        asio::io_context ioc;
        aqua::runtime::ClientRuntime client(ioc, cfg);
        if (!client.start()) {
            aqua::log_fatal("client failed to start; see preceding error for details");
            return 1;
        }

        const auto& cr = client.connect_result();
        aqua::log_info_fmt("client: session=0x{:08X} server={}:{} ({}ch/{}Hz/enc={}, F={})",
            cr.session_id, cr.udp_address, cr.udp_port,
            cr.audio_format.channels, cr.audio_format.sample_rate,
            static_cast<int>(cr.audio_format.encoding), cr.frame_count);

        aqua::diagnostics::Diagnostics diag("Client");
        diag.add_source("state", [&client]() {
            return std::format("state={}", aqua::runtime::runtime_state_name(client.state()));
        });
        diag.add_source("net", [&client]() {
            const auto s = client.udp_stats();
            return std::format("rx={} rxB={} rxerr={} tx={} txB={} txerr={} drop={} enqfail={} q={} ack={} misses={} ack_age_ms={} hello_failed={}",
                s.rx_packets, s.rx_bytes, s.rx_errors, s.tx_packets, s.tx_bytes, s.tx_errors,
                s.tx_dropped, s.tx_enqueue_failures, s.tx_queue_depth, client.hello_ack_count(),
                client.hello_ack_misses(), client.hello_ack_age_ms(), client.udp_hello_failed());
        });
        diag.add_source("jb", [&client]() {
            return std::format("water={:.2f} used={}/{} reanchor={} reanchor_req={} reanchor_cancel={} sanity_reject={} last={} push_ok={} push_reject={} late={} busy={} invalid={} pull_calls={} pull_frames={} silence_frames={} fill_episodes={} fill_frames={} drop_episodes={} skip_slots={}",
                client.jitter_water_level(), client.jitter_used_slots(), client.jitter_capacity_slots(),
                client.jitter_reanchor_count(), client.jitter_reanchor_requests(), client.jitter_reanchor_cancels(),
                client.jitter_reanchor_sanity_rejections(), client.jitter_last_reanchor_sequence(),
                client.jitter_push_accepted(), client.jitter_push_rejected(), client.jitter_push_rejected_late(),
                client.jitter_push_rejected_slot_busy(), client.jitter_push_rejected_invalid(),
                client.jitter_pull_calls(), client.jitter_pull_frames(), client.jitter_pull_silence_frames(),
                client.jitter_fill_episodes(), client.jitter_fill_hold_frames(),
                client.jitter_drop_episodes(), client.jitter_drop_skipped_slots());
        });
        diag.add_source("playback", [&client]() {
            return std::format("running={} audio_error={} pull_calls={} pull_frames={} silence_frames={}",
                client.playback_running(), aqua::audio::audio_error_name(client.last_audio_error()),
                client.playback_pull_calls(), client.playback_pull_frames(), client.playback_pull_silence_frames());
        });
        diag.add_counter("udp_audio", [&client]() { return client.udp_audio_frames_accepted(); });
        diag.add_counter("udp_malformed", [&client]() { return client.udp_malformed_datagrams(); });
        diag.add_counter("udp_unexpected_sender", [&client]() { return client.udp_unexpected_sender_datagrams(); });
        diag.add_counter("udp_wrong_session_ack", [&client]() { return client.udp_wrong_session_acks(); });
        diag.add_counter("udp_payload_mismatch", [&client]() { return client.udp_audio_payload_mismatches(); });
        diag.add_counter("udp_non_audio", [&client]() { return client.udp_non_audio_datagrams(); });
        diag.add_counter("hello_send_attempts", [&client]() { return client.udp_hello_send_attempts(); });
        diag.add_counter("hello_ack", [&client]() { return client.hello_ack_count(); });
        diag.add_counter("hello_ack_misses_total", [&client]() { return client.udp_hello_ack_miss_events(); });
        diag.add_counter("jb_push_accepted", [&client]() { return client.jitter_push_accepted(); });
        diag.add_counter("jb_push_rejected", [&client]() { return client.jitter_push_rejected(); });
        diag.add_counter("jb_pull_calls", [&client]() { return client.jitter_pull_calls(); });
        diag.add_counter("jb_pull_frames", [&client]() { return client.jitter_pull_frames(); });
        diag.add_counter("jb_silence", [&client]() { return client.jitter_pull_silence_frames(); });
        diag.add_counter("jb_reanchor", [&client]() { return client.jitter_reanchor_count(); });
        diag.add_counter("jb_reanchor_req", [&client]() { return client.jitter_reanchor_requests(); });
        diag.add_counter("playback_pull", [&client]() { return client.playback_pull_calls(); });
        diag.add_counter("playback_frames", [&client]() { return client.playback_pull_frames(); });
        diag.add_counter("playback_silence", [&client]() { return client.playback_pull_silence_frames(); });

        diag.add_counter("udp_rx_packets", [&client]() { return client.udp_stats().rx_packets; });
        diag.add_counter("udp_rx_bytes", [&client]() { return client.udp_stats().rx_bytes; });
        diag.add_counter("udp_rx_errors", [&client]() { return client.udp_stats().rx_errors; });
        diag.add_counter("udp_tx_packets", [&client]() { return client.udp_stats().tx_packets; });
        diag.add_counter("udp_tx_bytes", [&client]() { return client.udp_stats().tx_bytes; });
        diag.add_counter("udp_tx_errors", [&client]() { return client.udp_stats().tx_errors; });
        diag.add_counter("udp_tx_dropped", [&client]() { return client.udp_stats().tx_dropped; });
        diag.add_counter("udp_tx_enqueue_fail", [&client]() { return client.udp_stats().tx_enqueue_failures; });
        diag.add_counter("jb_push_late", [&client]() { return client.jitter_push_rejected_late(); });
        diag.add_counter("jb_push_busy", [&client]() { return client.jitter_push_rejected_slot_busy(); });
        diag.add_counter("jb_push_invalid", [&client]() { return client.jitter_push_rejected_invalid(); });
        diag.add_counter("jb_push_sanity", [&client]() { return client.jitter_push_rejected_sanity(); });
        diag.add_counter("jb_fill_episodes", [&client]() { return client.jitter_fill_episodes(); });
        diag.add_counter("jb_fill_frames", [&client]() { return client.jitter_fill_hold_frames(); });
        diag.add_counter("jb_drop_episodes", [&client]() { return client.jitter_drop_episodes(); });
        diag.add_counter("jb_skip_slots", [&client]() { return client.jitter_drop_skipped_slots(); });

        auto diag_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> diag_tick;
        diag_tick = [diag_timer, &diag, &diag_tick](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            diag.log_debug();
            diag_timer->expires_after(aqua::config::DIAGNOSTICS_SNAPSHOT_INTERVAL);
            diag_timer->async_wait(diag_tick);
        };
        diag_tick(asio::error_code {});
        aqua::log_debug("client: diagnostics snapshot interval=1000ms");

        auto control_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> control_tick;
        control_tick = [control_timer, &control_tick, &client, &ioc](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
                aqua::log_trace_fmt("client: control poll tick state={} hello_failed={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_hello_failed());
            }
            if (client.state() == aqua::runtime::RuntimeState::Degraded
                || client.udp_hello_failed()) {
                aqua::log_debug_fmt("client: control poll observed terminal condition: state={} hello_failed={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_hello_failed());
                client.stop();
                ioc.stop();
                return;
            }
            control_timer->expires_after(aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL);
            control_timer->async_wait(control_tick);
        };
        control_tick(asio::error_code {});

        #ifdef _WIN32
        asio::signal_set signals(ioc, SIGINT, SIGTERM, SIGBREAK);
        #else
        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        #endif
        signals.async_wait([&](const asio::error_code& ec, int signal_number) {
            if (!ec) {
                aqua::log_info_fmt("client: shutdown requested by signal {}", signal_number);
            }
            client.stop();
            ioc.stop();
        });

        ioc.run();

        client.stop();

        aqua::log_info("client: stopped");
        return 0;
    } catch (const std::exception& e) {
        aqua::log_fatal_fmt("ClientRuntime fatal error: {}", e.what());
        return 1;
    } catch (...) {
        aqua::log_fatal("ClientRuntime fatal error: unknown exception");
        return 1;
    }
}
