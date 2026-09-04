// aqua_server_cli：完整 server。参数解析在 cli_parser_server，装配与生命周期在 ServerRuntime。

#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
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
        aqua::log_debug_fmt("CLI config: advertise_udp={}",
            aqua::net::format_host_port(
                cfg.advertised_udp_address.empty() ? cfg.server_ip : cfg.advertised_udp_address,
                cfg.advertised_udp_port.value_or(cfg.udp_port)));

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
        aqua::log_info_fmt("server: bind {} gRPC:{} udp:{} advertise={} ({}ch/{}Hz/enc={}, F={})",
            cfg.server_ip, cfg.rpc_port, server->udp_port(),
            aqua::net::format_host_port(advertised_udp_ip, advertised_udp_port),
            server->audio_format().channels, server->audio_format().sample_rate,
            static_cast<int>(server->audio_format().encoding), server->frame_count());

        // 诊断源统一读聚合快照（aqua::diagnostics::ServerDiagnosticsSnapshot）：diag tick
        // 先刷新一次，保证同一行内各分组来自同一份近似读值。tick 与 Diagnostics 求值
        // 都在 io_context 线程上顺序执行，无并发访问。
        auto snapshot = std::make_shared<aqua::diagnostics::ServerDiagnosticsSnapshot>(
            server->take_diagnostics_snapshot());
        aqua::diagnostics::Diagnostics diag("Server");
        diag.add_source("state", [snapshot, &server]() {
            return std::format("state={} sessions={} udp_port={}",
                aqua::runtime::runtime_state_name(snapshot->state),
                snapshot->session.active, server->udp_port());
        });
        diag.add_source("audio", [snapshot, &cfg]() {
            const auto& cs = snapshot->capture_switch;
            return std::format("capture={} error={} format={}ch/{}Hz/enc={} F={} source={} capture_state={} switch={} route={}{} last_switch={}/{}ms cap_frames={} cap_bytes={} pkt_last={} pkt_min={} pkt_max={} starved_now_ms={} starved_max_ms={}",
                snapshot->capture_running,
                aqua::audio::audio_error_name(snapshot->last_audio_error),
                snapshot->audio_format.channels, snapshot->audio_format.sample_rate,
                static_cast<int>(snapshot->audio_format.encoding), snapshot->frame_count,
                static_cast<int>(cfg.capture.source),
                aqua::audio::capture_state_name(snapshot->capture.state),
                aqua::audio::capture_switch_state_name(cs.state),
                aqua::audio::capture_route_mode_name(cs.route),
                cs.route == aqua::audio::CaptureRouteMode::PreferredDevice
                    ? std::format("({})", cs.requested_device_id)
                    : std::string { },
                aqua::audio::switch_outcome_name(cs.last_outcome), cs.last_switch_duration_ms,
                snapshot->capture.captured_frames, snapshot->capture.captured_bytes,
                snapshot->capture.packet_frames_last, snapshot->capture.packet_frames_min,
                snapshot->capture.packet_frames_max,
                snapshot->capture.current_starved_ms, snapshot->capture.max_starved_ms);
        });
        diag.add_source("queue", [snapshot]() {
            return std::format("depth={} hwm={}", snapshot->queue.depth_slots,
                snapshot->queue.high_watermark_slots);
        });
        diag.add_source("sessions", [snapshot]() {
            const auto& s = snapshot->session;
            return std::format("active={} created={} connected={} refreshed={} removed={} expired={} clear_removed={}",
                s.active, s.created, s.connected, s.refreshed, s.removed, s.expired, s.clear_removed);
        });
        diag.add_source("udp", [snapshot]() {
            const auto& s = snapshot->net.transport;
            return std::format("rx={} rxB={} rxerr={} tx={} txB={} txerr={} drop={} enqfail={} q={}",
                s.rx_packets, s.rx_bytes, s.rx_errors, s.tx_packets, s.tx_bytes,
                s.tx_errors, s.tx_dropped, s.tx_enqueue_failures, s.tx_queue_depth);
        });
        diag.add_counter("capture_blocks", [snapshot]() { return snapshot->packetizer.input_blocks; });
        diag.add_counter("capture_bytes", [snapshot]() { return snapshot->packetizer.input_bytes; });
        diag.add_counter("capture_events", [snapshot]() { return snapshot->capture.audio_events; });
        diag.add_counter("capture_packet_queries", [snapshot]() { return snapshot->capture.packet_queries; });
        diag.add_counter("capture_packet_empty", [snapshot]() { return snapshot->capture.packet_empty; });
        diag.add_counter("capture_packets_ready", [snapshot]() { return snapshot->capture.packets_ready; });
        diag.add_counter("capture_get_buffer", [snapshot]() { return snapshot->capture.get_buffer_success; });
        diag.add_counter("capture_callbacks", [snapshot]() { return snapshot->capture.callbacks; });
        diag.add_counter("capture_silent_callbacks", [snapshot]() { return snapshot->capture.silent_callbacks; });
        diag.add_counter("capture_synthetic_blocks", [snapshot]() { return snapshot->capture.synthetic_silence_blocks; });
        diag.add_counter("capture_generated_silence_frames", [snapshot]() { return snapshot->capture.generated_silence_frames; });
        diag.add_counter("capture_starved_events", [snapshot]() { return snapshot->capture.starved_events; });
        diag.add_counter("capture_starved_ms", [snapshot]() { return snapshot->capture.starved_ms; });
        diag.add_counter("capture_captured_frames", [snapshot]() { return snapshot->capture.captured_frames; });
        diag.add_counter("capture_captured_bytes", [snapshot]() { return snapshot->capture.captured_bytes; });
        diag.add_counter("packetizer_unaligned", [snapshot]() { return snapshot->packetizer.rejected_unaligned_blocks; });
        diag.add_counter("packetizer_pending_discards", [snapshot]() { return snapshot->packetizer.pending_discards; });
        diag.add_counter("packetizer_frames", [snapshot]() { return snapshot->packetizer.frames_emitted; });
        diag.add_counter("queue_accepted", [snapshot]() { return snapshot->queue.accepted_frames; });
        diag.add_counter("queue_consumed", [snapshot]() { return snapshot->queue.consumed_frames; });
        diag.add_counter("queue_dropped", [snapshot]() { return snapshot->queue.dropped_frames; });
        diag.add_counter("published_frames", [snapshot]() { return snapshot->dispatcher.published_frames; });
        diag.add_counter("dispatcher_wakeups", [snapshot]() { return snapshot->dispatcher.worker_wakeups; });
        diag.add_counter("frames_encoded", [snapshot]() { return snapshot->dispatcher.frames_encoded; });
        diag.add_counter("frames_broadcast", [snapshot]() { return snapshot->dispatcher.frames_broadcast; });
        diag.add_counter("frames_no_clients", [snapshot]() { return snapshot->dispatcher.frames_without_clients; });
        diag.add_counter("encode_fail", [snapshot]() { return snapshot->dispatcher.encode_failures; });
        diag.add_counter("dispatch_fail", [snapshot]() { return snapshot->dispatcher.dispatch_failures; });
        diag.add_counter("network_queue_drop", [snapshot]() { return snapshot->dispatcher.dropped_frames; });

        diag.add_counter("udp_rx_packets", [snapshot]() { return snapshot->net.transport.rx_packets; });
        diag.add_counter("udp_rx_bytes", [snapshot]() { return snapshot->net.transport.rx_bytes; });
        diag.add_counter("udp_rx_errors", [snapshot]() { return snapshot->net.transport.rx_errors; });
        diag.add_counter("udp_tx_packets", [snapshot]() { return snapshot->net.transport.tx_packets; });
        diag.add_counter("udp_tx_bytes", [snapshot]() { return snapshot->net.transport.tx_bytes; });
        diag.add_counter("udp_tx_errors", [snapshot]() { return snapshot->net.transport.tx_errors; });
        diag.add_counter("udp_tx_dropped", [snapshot]() { return snapshot->net.transport.tx_dropped; });
        diag.add_counter("udp_tx_enqueue_fail", [snapshot]() { return snapshot->net.transport.tx_enqueue_failures; });
        diag.add_counter("udp_hello_received", [snapshot]() { return snapshot->net.hello_received; });
        diag.add_counter("udp_hello_rejected", [snapshot]() { return snapshot->net.hello_rejected; });
        diag.add_counter("udp_sessions_established", [snapshot]() { return snapshot->net.sessions_established; });
        diag.add_counter("udp_sessions_refreshed", [snapshot]() { return snapshot->net.sessions_refreshed; });
        diag.add_counter("udp_hello_ack_attempts", [snapshot]() { return snapshot->net.hello_ack_attempts; });
        diag.add_counter("udp_malformed", [snapshot]() { return snapshot->net.malformed_datagrams; });
        diag.add_counter("udp_non_hello", [snapshot]() { return snapshot->net.non_hello_datagrams; });
        diag.add_counter("session_created", [snapshot]() { return snapshot->session.created; });
        diag.add_counter("session_connected", [snapshot]() { return snapshot->session.connected; });
        diag.add_counter("session_refreshed", [snapshot]() { return snapshot->session.refreshed; });
        diag.add_counter("session_removed", [snapshot]() { return snapshot->session.removed; });
        diag.add_counter("session_expired", [snapshot]() { return snapshot->session.expired; });
        auto diag_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> diag_tick;
        diag_tick = [diag_timer, &diag, &diag_tick, snapshot, &server](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            *snapshot = server->take_diagnostics_snapshot();
            diag.log_debug();
            diag_timer->expires_after(aqua::config::DIAGNOSTICS_SNAPSHOT_INTERVAL);
            diag_timer->async_wait(diag_tick);
        };
        diag_tick(asio::error_code { });
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
            // capture 切换决策（capture_switching_design.md §6：决策者 = CLI
            // control timer；决策表由 runtime 执行）：设备错误 -> restart 候选链；
            // FollowSystem 轮询默认设备变化并跟随；Fatal（链耗尽/预算超限）
            // 是唯一新增终止条件——无 capture 的会话无意义。
            const auto action = server->service_capture_switching();
            if (action == aqua::runtime::ServerRuntime::CaptureServiceAction::Fatal) {
                aqua::log_error("server: capture switch fatal (fallback chain exhausted), stopping");
                server->stop();
                ioc.stop();
                return;
            }
            control_timer->expires_after(aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL);
            control_timer->async_wait(control_tick);
        };
        control_tick(asio::error_code { });

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
