// aqua_server_cli：完整 server。参数解析在 cli_parser_server，装配与生命周期在 ServerRuntime。

#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/runtime/server_runtime.h"
#include "aqua/net/address/address_utils.h"

#include "cli_parser/cli_parser_server.h"

#include <asio.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    aqua::cli::configure_console_utf8();
    aqua::runtime::ServerRuntimeConfig cfg;
    aqua::LogLevel log_level = aqua::default_log_level();
    if (const auto exit_code = aqua::cli::cli_exit_code(
            aqua::cli::parse_server_cli(argc, argv, cfg, log_level))) {
        return *exit_code;
    }

    try {
        aqua::init_logger();
        aqua::set_log_level(log_level);
        aqua::log_debug_fmt(
            "CLI config: log_level={} server_ip={} rpc_port={} udp_port={} advertise={} format={}ch/{}Hz/enc={} frame_count={} queue_slots={} capture_source={} capture_device={}",
            aqua::log_level_name(log_level), cfg.server_ip, cfg.rpc_port, cfg.udp_port,
            cfg.advertised_udp_address.empty() ? cfg.server_ip : cfg.advertised_udp_address,
            cfg.format ? cfg.format->channels : 0, cfg.format ? cfg.format->sample_rate : 0,
            cfg.format ? static_cast<int>(cfg.format->encoding) : static_cast<int>(aqua::audio::AudioEncoding::INVALID),
            cfg.frame_count, cfg.network_queue_slots,
            static_cast<int>(cfg.capture.source), cfg.capture.device ? cfg.capture.device->value() : std::string("default"));
        aqua::log_debug_fmt("CLI config: advertise_udp={}:{}",
            cfg.advertised_udp_address.empty() ? cfg.server_ip : cfg.advertised_udp_address,
            cfg.advertised_udp_port.value_or(cfg.udp_port));

        asio::io_context ioc;
        auto server = std::make_shared<aqua::runtime::ServerRuntime>(ioc, cfg);
        if (!server->start()) {
            aqua::log_fatal("server failed to start; see preceding error for details");
            return 1;
        }

        const auto advertised_udp_ip = cfg.advertised_udp_address.empty()
            ? cfg.server_ip
            : cfg.advertised_udp_address;
        const auto advertised_udp_port = cfg.advertised_udp_port.value_or(server->udp_port());
        aqua::log_info_fmt("server: bind {} gRPC:{} udp:{} advertise={}:{} ({}ch/{}Hz/enc={}, F={})",
            cfg.server_ip, cfg.rpc_port, server->udp_port(), advertised_udp_ip, advertised_udp_port,
            server->audio_format().channels, server->audio_format().sample_rate,
            static_cast<int>(server->audio_format().encoding), server->frame_count());

        aqua::diagnostics::Diagnostics diag("Server");
        diag.add_source("state", [&server]() {
            return std::format("state={} sessions={} udp_port={}",
                aqua::runtime::runtime_state_name(server->state()), server->session_count(), server->udp_port());
        });
        diag.add_source("audio", [&server, &cfg]() {
            return std::format("capture={} error={} format={}ch/{}Hz/enc={} F={} source={} capture_state={}",
                server->capture_running(), aqua::audio::audio_error_name(server->last_audio_error()),
                server->audio_format().channels, server->audio_format().sample_rate,
                static_cast<int>(server->audio_format().encoding), server->frame_count(),
                static_cast<int>(cfg.capture.source),
                aqua::audio::capture_state_name(server->capture_stats().state));
        });
        diag.add_source("queue", [&server]() {
            return std::format("depth={}", server->queue_depth());
        });
        diag.add_source("sessions", [&server]() {
            const auto s = server->session_stats();
            return std::format("active={} created={} connected={} refreshed={} removed={} expired={} clear_removed={}",
                server->session_count(), s.created, s.connected, s.refreshed, s.removed, s.expired, s.clear_removed);
        });
        diag.add_source("udp", [&server]() {
            const auto s = server->udp_stats();
            return std::format("rx={} rxB={} rxerr={} tx={} txB={} txerr={} drop={} enqfail={} q={}",
                s.rx_packets, s.rx_bytes, s.rx_errors, s.tx_packets, s.tx_bytes,
                s.tx_errors, s.tx_dropped, s.tx_enqueue_failures, s.tx_queue_depth);
        });
        diag.add_counter("capture_blocks", [&server]() { return server->packetizer_input_blocks(); });
        diag.add_counter("capture_bytes", [&server]() { return server->packetizer_input_bytes(); });
        diag.add_counter("capture_events", [&server]() { return server->capture_stats().audio_events; });
        diag.add_counter("capture_packet_queries", [&server]() { return server->capture_stats().packet_queries; });
        diag.add_counter("capture_packet_empty", [&server]() { return server->capture_stats().packet_empty; });
        diag.add_counter("capture_packets_ready", [&server]() { return server->capture_stats().packets_ready; });
        diag.add_counter("capture_get_buffer", [&server]() { return server->capture_stats().get_buffer_success; });
        diag.add_counter("capture_callbacks", [&server]() { return server->capture_stats().callbacks; });
        diag.add_counter("capture_silent_callbacks", [&server]() { return server->capture_stats().silent_callbacks; });
        diag.add_counter("capture_synthetic_blocks", [&server]() { return server->capture_stats().synthetic_silence_blocks; });
        diag.add_counter("capture_generated_silence_frames", [&server]() { return server->capture_stats().generated_silence_frames; });
        diag.add_counter("capture_starved_events", [&server]() { return server->capture_stats().starved_events; });
        diag.add_counter("capture_starved_ms", [&server]() { return server->capture_stats().starved_ms; });
        diag.add_counter("packetizer_unaligned", [&server]() { return server->packetizer_rejected_unaligned_blocks(); });
        diag.add_counter("packetizer_frames", [&server]() { return server->packetizer_frames_emitted(); });
        diag.add_counter("queue_accepted", [&server]() { return server->queue_accepted_frames(); });
        diag.add_counter("queue_consumed", [&server]() { return server->queue_consumed_frames(); });
        diag.add_counter("queue_dropped", [&server]() { return server->queue_dropped_frames(); });
        diag.add_counter("published_frames", [&server]() { return server->dispatcher_published_frames(); });
        diag.add_counter("dispatcher_wakeups", [&server]() { return server->dispatcher_worker_wakeups(); });
        diag.add_counter("frames_encoded", [&server]() { return server->frames_encoded(); });
        diag.add_counter("frames_broadcast", [&server]() { return server->frames_broadcast(); });
        diag.add_counter("frames_no_clients", [&server]() { return server->frames_without_clients(); });
        diag.add_counter("encode_fail", [&server]() { return server->encode_failures(); });
        diag.add_counter("dispatch_fail", [&server]() { return server->dispatch_failures(); });
        diag.add_counter("network_queue_drop", [&server]() { return server->frames_dropped_before_network(); });

        diag.add_counter("udp_rx_packets", [&server]() { return server->udp_stats().rx_packets; });
        diag.add_counter("udp_rx_bytes", [&server]() { return server->udp_stats().rx_bytes; });
        diag.add_counter("udp_rx_errors", [&server]() { return server->udp_stats().rx_errors; });
        diag.add_counter("udp_tx_packets", [&server]() { return server->udp_stats().tx_packets; });
        diag.add_counter("udp_tx_bytes", [&server]() { return server->udp_stats().tx_bytes; });
        diag.add_counter("udp_tx_errors", [&server]() { return server->udp_stats().tx_errors; });
        diag.add_counter("udp_tx_dropped", [&server]() { return server->udp_stats().tx_dropped; });
        diag.add_counter("udp_tx_enqueue_fail", [&server]() { return server->udp_stats().tx_enqueue_failures; });
        diag.add_counter("udp_hello_received", [&server]() { return server->udp_hello_received(); });
        diag.add_counter("udp_hello_rejected", [&server]() { return server->udp_hello_rejected(); });
        diag.add_counter("udp_sessions_established", [&server]() { return server->udp_sessions_established(); });
        diag.add_counter("udp_sessions_refreshed", [&server]() { return server->udp_sessions_refreshed(); });
        diag.add_counter("udp_hello_ack_attempts", [&server]() { return server->udp_hello_ack_attempts(); });
        diag.add_counter("udp_malformed", [&server]() { return server->udp_malformed_datagrams(); });
        diag.add_counter("udp_non_hello", [&server]() { return server->udp_non_hello_datagrams(); });
        diag.add_counter("session_created", [&server]() { return server->session_stats().created; });
        diag.add_counter("session_connected", [&server]() { return server->session_stats().connected; });
        diag.add_counter("session_refreshed", [&server]() { return server->session_stats().refreshed; });
        diag.add_counter("session_removed", [&server]() { return server->session_stats().removed; });
        diag.add_counter("session_expired", [&server]() { return server->session_stats().expired; });
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

        #ifdef _WIN32
        asio::signal_set signals(ioc, SIGINT, SIGTERM, SIGBREAK);
        #else
        asio::signal_set signals(ioc, SIGINT, SIGTERM);
        #endif
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
    } catch (const std::exception& e) {
        aqua::log_fatal_fmt("ServerRuntime fatal error: {}", aqua::format_exception_message(e));
        return 1;
    } catch (...) {
        aqua::log_fatal("ServerRuntime fatal error: unknown exception");
        return 1;
    }
}
