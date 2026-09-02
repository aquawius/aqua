// aqua_client_cli：完整 client。参数解析在 cli_parser_client，装配与生命周期在 ClientRuntime。

#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/runtime/client_runtime.h"

#include "cli_parser/cli_parser_client.h"

#include <asio.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <memory>

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
        aqua::log_debug_fmt("CLI config: log_level={} server={} client_name='{}' jitter_slots={} hello_interval={}ms force_udp_port={} playback_device={} playback_buffer_frames={}",
            aqua::log_level_name(log_level), aqua::net::format_host_port(cfg.server_ip, cfg.rpc_port), cfg.client_name,
            cfg.jitter_buffer_slots, cfg.hello_interval.count(),
            cfg.force_udp_port ? std::to_string(*cfg.force_udp_port) : std::string("server-advertised"),
            cfg.playback.device ? cfg.playback.device->value() : std::string("default"),
            cfg.playback.frames_per_buffer);

        asio::io_context ioc;
        aqua::runtime::ClientRuntime client(ioc, cfg);
        if (!client.start()) {
            aqua::log_fatal("client failed to start; see preceding error for details");
            return 1;
        }

        const auto& cr = client.connect_result();
        aqua::log_info_fmt("client: session=0x{:08X} server={} ({}ch/{}Hz/enc={}, F={})",
            cr.session_id, aqua::net::format_host_port(cr.advertised_udp_address, cr.advertised_udp_port),
            cr.audio_format.channels, cr.audio_format.sample_rate,
            static_cast<int>(cr.audio_format.encoding), cr.frame_count);

        // 诊断源统一读聚合快照（aqua::diagnostics::ClientDiagnosticsSnapshot）：diag tick
        // 先刷新一次，保证同一行内 state/net/jb/playback 各分组来自同一份近似读值。
        // tick 与 Diagnostics 求值都在 io_context 线程上顺序执行，无并发访问。
        auto snapshot = std::make_shared<aqua::diagnostics::ClientDiagnosticsSnapshot>(
            client.take_diagnostics_snapshot());
        aqua::diagnostics::Diagnostics diag("Client");
        diag.add_source("state", [snapshot]() {
            return std::format("state={}",
                aqua::runtime::runtime_state_name(snapshot->state));
        });
        diag.add_source("net", [snapshot]() {
            const auto& s = snapshot->net;
            return std::format("rx={} rxB={} rxerr={} tx={} txB={} txerr={} drop={} enqfail={} q={} ack={} misses={} ack_age_ms={} hello_failed={}",
                s.transport.rx_packets, s.transport.rx_bytes, s.transport.rx_errors,
                s.transport.tx_packets, s.transport.tx_bytes, s.transport.tx_errors,
                s.transport.tx_dropped, s.transport.tx_enqueue_failures,
                s.transport.tx_queue_depth, s.hello_ack_count, s.hello_ack_misses,
                s.hello_ack_age_ms, s.hello_failed);
        });
        diag.add_source("jb", [snapshot]() {
            const auto& jb = snapshot->jitter_buffer;
            return std::format("water={:.2f} used={}/{} reanchor={} reanchor_req={} reanchor_cancel={} sanity_reject={} last={} push_ok={} push_reject={} late={} busy={} invalid={} pull_calls={} pull_frames={} silence_frames={} fill_episodes={} fill_slots={} drop_episodes={} skip_slots={}",
                jb.water_level, jb.used_slots, jb.capacity_slots,
                jb.reanchor_count, jb.reanchor_requests, jb.reanchor_cancels,
                jb.reanchor_sanity_rejections, jb.last_reanchor_sequence,
                jb.push_accepted, jb.push_rejected, jb.push_rejected_late,
                jb.push_rejected_slot_busy, jb.push_rejected_invalid,
                jb.pull_calls, jb.pull_frames, jb.pull_silence_frames,
                jb.fill_episodes, jb.fill_corrected_slots,
                jb.drop_episodes, jb.drop_skipped_slots);
        });
        diag.add_source("playback", [snapshot]() {
            return std::format("running={} playback_state={} audio_error={} pull_calls={} pull_frames={} silence_frames={}",
                snapshot->playback_running,
                aqua::audio::playback_state_name(snapshot->playback_state),
                aqua::audio::audio_error_name(snapshot->last_audio_error),
                snapshot->playback.pull_calls, snapshot->playback.pull_frames,
                snapshot->playback.pull_silence_frames);
        });
        diag.add_source("stream", [snapshot]() {
            const auto& s = snapshot->stream;
            return std::format("backend={} rate={} ch={} performance={} frames_per_burst={} capacity={}",
                aqua::audio::audio_stream_backend_name(s.backend),
                s.sample_rate, s.channels,
                aqua::audio::audio_stream_performance_name(s.performance_mode),
                s.frames_per_burst, s.buffer_capacity_frames);
        });
        diag.add_counter("udp_audio", [snapshot]() { return snapshot->net.audio_frames_accepted; });
        diag.add_counter("udp_malformed", [snapshot]() { return snapshot->net.malformed_datagrams; });
        diag.add_counter("udp_unexpected_sender", [snapshot]() { return snapshot->net.unexpected_sender_datagrams; });
        diag.add_counter("udp_wrong_session_ack", [snapshot]() { return snapshot->net.wrong_session_acks; });
        diag.add_counter("udp_payload_mismatch", [snapshot]() { return snapshot->net.audio_payload_mismatches; });
        diag.add_counter("udp_non_audio", [snapshot]() { return snapshot->net.non_audio_datagrams; });
        diag.add_counter("hello_send_attempts", [snapshot]() { return snapshot->net.hello_send_attempts; });
        diag.add_counter("hello_ack", [snapshot]() { return snapshot->net.hello_ack_count; });
        diag.add_counter("hello_ack_misses_total", [snapshot]() { return snapshot->net.hello_ack_miss_events; });
        diag.add_counter("jb_push_accepted", [snapshot]() { return snapshot->jitter_buffer.push_accepted; });
        diag.add_counter("jb_push_rejected", [snapshot]() { return snapshot->jitter_buffer.push_rejected; });
        diag.add_counter("jb_pull_calls", [snapshot]() { return snapshot->jitter_buffer.pull_calls; });
        diag.add_counter("jb_pull_frames", [snapshot]() { return snapshot->jitter_buffer.pull_frames; });
        diag.add_counter("jb_silence", [snapshot]() { return snapshot->jitter_buffer.pull_silence_frames; });
        diag.add_counter("jb_reanchor", [snapshot]() { return snapshot->jitter_buffer.reanchor_count; });
        diag.add_counter("jb_reanchor_req", [snapshot]() { return snapshot->jitter_buffer.reanchor_requests; });
        diag.add_counter("playback_pull", [snapshot]() { return snapshot->playback.pull_calls; });
        diag.add_counter("playback_frames", [snapshot]() { return snapshot->playback.pull_frames; });
        diag.add_counter("playback_silence", [snapshot]() { return snapshot->playback.pull_silence_frames; });

        diag.add_counter("udp_rx_packets", [snapshot]() { return snapshot->net.transport.rx_packets; });
        diag.add_counter("udp_rx_bytes", [snapshot]() { return snapshot->net.transport.rx_bytes; });
        diag.add_counter("udp_rx_errors", [snapshot]() { return snapshot->net.transport.rx_errors; });
        diag.add_counter("udp_tx_packets", [snapshot]() { return snapshot->net.transport.tx_packets; });
        diag.add_counter("udp_tx_bytes", [snapshot]() { return snapshot->net.transport.tx_bytes; });
        diag.add_counter("udp_tx_errors", [snapshot]() { return snapshot->net.transport.tx_errors; });
        diag.add_counter("udp_tx_dropped", [snapshot]() { return snapshot->net.transport.tx_dropped; });
        diag.add_counter("udp_tx_enqueue_fail", [snapshot]() { return snapshot->net.transport.tx_enqueue_failures; });
        diag.add_counter("jb_push_late", [snapshot]() { return snapshot->jitter_buffer.push_rejected_late; });
        diag.add_counter("jb_push_busy", [snapshot]() { return snapshot->jitter_buffer.push_rejected_slot_busy; });
        diag.add_counter("jb_push_invalid", [snapshot]() { return snapshot->jitter_buffer.push_rejected_invalid; });
        diag.add_counter("jb_push_sanity", [snapshot]() { return snapshot->jitter_buffer.push_rejected_sanity; });
        diag.add_counter("jb_fill_episodes", [snapshot]() { return snapshot->jitter_buffer.fill_episodes; });
        diag.add_counter("jb_fill_slots", [snapshot]() { return snapshot->jitter_buffer.fill_corrected_slots; });
        diag.add_counter("jb_drop_episodes", [snapshot]() { return snapshot->jitter_buffer.drop_episodes; });
        diag.add_counter("jb_skip_slots", [snapshot]() { return snapshot->jitter_buffer.drop_skipped_slots; });

        auto diag_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> diag_tick;
        diag_tick = [diag_timer, &diag, &diag_tick, snapshot, &client](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            *snapshot = client.take_diagnostics_snapshot();
            diag.log_debug();
            diag_timer->expires_after(aqua::config::DIAGNOSTICS_SNAPSHOT_INTERVAL);
            diag_timer->async_wait(diag_tick);
        };
        diag_tick(asio::error_code { });
        aqua::log_debug("client: diagnostics snapshot interval=1000ms");

        auto control_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> control_tick;
        control_tick = [control_timer, &control_tick, &client, &ioc](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
                aqua::log_trace_fmt("client: control poll tick state={} hello_failed={} playback_state={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_hello_failed(),
                    aqua::audio::playback_state_name(client.playback_state()));
            }
            // 错误驱动的播放恢复（playback_switching_design.md §6）：
            // 设备错误由本控制线程执行 restart 事务；链耗尽 → Fatal。
            client.service_playback_recovery();
            if (client.state() == aqua::runtime::RuntimeState::Degraded
                || client.udp_hello_failed()
                || client.playback_state() == aqua::audio::PlaybackState::Fatal) {
                aqua::log_debug_fmt("client: control poll observed terminal condition: state={} hello_failed={} playback_state={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_hello_failed(),
                    aqua::audio::playback_state_name(client.playback_state()));
                client.stop();
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
        aqua::log_fatal_fmt("ClientRuntime fatal error: {}", aqua::format_exception_message(e));
        return 1;
    } catch (...) {
        aqua::log_fatal("ClientRuntime fatal error: unknown exception");
        return 1;
    }
}
