// aqua_client_cli：完整 client。参数解析在 cli_parser_client，装配与生命周期在 ClientRuntime。

#include "aqua/audio/buffer/target_controller.h"
#include "aqua/diagnostics/diag_view.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/runtime/client_runtime.h"

#include "cli_parser/cli_parser_client.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <stop_token>
#include <thread>

int main(int argc, char** argv)
{
    // LogDrain 必须最先声明：按逆序析构，它在所有 worker（diag_thread /
    // runtime）join 之后最后执行同步排空，保证关机尾部不断行。
    struct LogDrain {
        ~LogDrain() { aqua::shutdown_logger(); }
    };
    const LogDrain log_drain;
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
        aqua::log_debug_fmt("CLI config: log_level={} server={} client_name='{}' jb_capacity={} heartbeat_handshake_interval={}ms udp_force_port={} playback_device={} playback_buffer_frames={} jb_adaptive_target={} jb_jitter_gain={:.2f} jb_min_target={} jb_pcm_concealment={}",
            aqua::log_level_name(log_level), aqua::net::format_host_port(cfg.server_ip, cfg.rpc_port), cfg.client_name,
            cfg.jb_capacity_slots, cfg.heartbeat_handshake_interval.count(),
            cfg.udp_force_port ? std::to_string(*cfg.udp_force_port) : std::string("server-advertised"),
            cfg.playback.device ? cfg.playback.device->value() : std::string("default"),
            cfg.playback.frames_per_buffer, cfg.jb_adaptive_target, cfg.jb_jitter_gain,
            cfg.jb_min_target_slots, cfg.jb_pcm_concealment);
        // --jb-* 的有效取值与来源：一行顶一次"参数到底生效没有"的问答。
        // 有意**不**挂在 AQUA_JB_CONTROL_THREAD_DEBUG_LOG 下——它是启动期一次性
        // 诊断，而 Release 现场（两个调试宏都关）恰恰最需要它。后缀 cli =
        // 命令行显式指定，default = 取 buffer_config.h 的默认值。
        {
            const auto& prov = cfg.jb_option_provenance;
            aqua::log_debug_fmt(
                "CLI effective JB options (src: cli=explicit, default=built-in): capacity={}({}) jitter_gain={:g}({}) min_target={}({}) stall_peak_cap={:g}({}) stall_decay={:g}({}) stall_threshold={:g}({}) underrun_penalty={:g}({}) adaptive={} concealment={}",
                cfg.jb_capacity_slots, prov.capacity ? "cli" : "default",
                cfg.jb_jitter_gain, prov.jitter_gain ? "cli" : "default",
                cfg.jb_min_target_slots, prov.min_target ? "cli" : "default",
                cfg.jb_stall_peak_cap_slots, prov.stall_peak_cap ? "cli" : "default",
                cfg.jb_stall_peak_decay_ms_per_sec, prov.stall_decay ? "cli" : "default",
                cfg.jb_stall_threshold_packets, prov.stall_threshold ? "cli" : "default",
                cfg.jb_underrun_penalty_slots, prov.underrun_penalty ? "cli" : "default",
                cfg.jb_adaptive_target, cfg.jb_pcm_concealment);
        }

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
        // tick 与 Diagnostics 求值在专用 diag 线程上顺序执行（不得占用网络 ioc，
        // 见下方 diag_thread 注释），快照内只有该线程一个读写者。
        auto snapshot = std::make_shared<aqua::diagnostics::ClientDiagnosticsSnapshot>(
            client.take_diagnostics_snapshot());
        aqua::diagnostics::ClientDiagView diag_view;
        aqua::diagnostics::Diagnostics diag("Client");
        // 每拍刷新一次快照（state/net/jb/jc/pb/stream 同口径），渲染交给 ClientDiagView
        // 的持久 RateCounter——各模块诊断 State 收敛进 DiagView，读法见 doc/diagnostics.md。
        diag.add_source("state", [&] { return diag_view.render_state(*snapshot); });
        diag.add_source("net", [&] { return diag_view.render_net(*snapshot); });
        diag.add_source("jb", [&] { return diag_view.render_jb(*snapshot); });
        diag.add_source("jc", [&] { return diag_view.render_jc(*snapshot); });
        diag.add_source("pb", [&] { return diag_view.render_playback(*snapshot, aqua::audio::audio_error_name(client.last_audio_error())); });
        diag.add_source("stream", [&] { return diag_view.render_stream(*snapshot); });

        // 诊断 tick 用独立线程而非 ioc 定时器：即便诊断行已压缩成紧凑块，同步写
        // 控制台（Windows 控制台写可阻塞数 ms ~ 数十 ms）仍会卡住网络收发
        //（async_receive 派发 / tx strand 泵）。独立线程承担格式化与写盘，避免在
        // 网络链路上制造周期性收包空隙。
        // take_diagnostics_snapshot / last_audio_error 是 C API 契约、任意线程
        // 可调，独立线程调用无并发问题。
        // std::jthread：析构时自动 request_stop + join，取代原先的
        // atomic<bool> 停止旗 + 手写 DiagThreadJoin RAII。
        // 声明顺序保证 diag_thread 先于其引用的 client / snapshot / diag 析构。
        std::jthread diag_thread([&](std::stop_token st) {
            while (!st.stop_requested()) {
                *snapshot = client.take_diagnostics_snapshot();
                diag.log_debug();
                // 分片睡眠：停止请求至多晚一个分片（50ms）被观察到。
                for (int i = 0; i < 20 && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(
                        aqua::config::DIAGNOSTICS_SNAPSHOT_INTERVAL / 20);
                }
            }
        });
        aqua::log_debug("client: diagnostics snapshot interval=1000ms (dedicated thread)");

        auto control_timer = std::make_shared<asio::steady_timer>(ioc);
        std::function<void(const asio::error_code&)> control_tick;
        control_tick = [control_timer, &control_tick, &client, &ioc](const asio::error_code& ec) {
            if (ec) {
                return;
            }
            if (aqua::log_level_enabled(aqua::LogLevel::Trace)) {
                aqua::log_trace_fmt("client: control poll tick state={} heartbeat_failed={} playback_state={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_heartbeat_failed(),
                    aqua::audio::playback_state_name(client.playback_state()));
            }
            // 错误驱动的播放恢复 + 默认设备跟随 + 终态裁决（单实现见
            // ClientRuntime::poll_control；双致命都经 handler 置 Degraded，
            // heartbeat_failed 锁存与 Degraded 同 tick 产生，此处只看裁决）。
            const auto verdict = client.poll_control();
            if (verdict != aqua::runtime::ClientRuntime::ControlPoll::Continue) {
                aqua::log_debug_fmt("client: control poll observed terminal condition: state={} heartbeat_failed={} playback_state={}",
                    aqua::runtime::runtime_state_name(client.state()), client.udp_heartbeat_failed(),
                    aqua::audio::playback_state_name(client.playback_state()));
                if (verdict == aqua::runtime::ClientRuntime::ControlPoll::StopDegraded) {
                    aqua::log_info("client: network degraded, exiting");
                } else {
                    aqua::log_info("client: playback fatal, exiting");
                }
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
        // 两段式关闭（与 server_main 对称）：第一次优雅停止，第二次强制退出。
        // 此前这里是 one-shot：第二次 Ctrl+C 走默认处置直接杀进程，
        // 正好落在 teardown 的 join 空窗里，关机尾部断行。重挂后第二次信号
        // 可控：先排空日志再 _Exit，不断行。
        static std::atomic<int> signal_count { 0 };
        std::function<void(const asio::error_code&, int)> on_signal;
        on_signal = [&](const asio::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }
            signals.async_wait(on_signal);
            const int n = signal_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                aqua::log_info_fmt("client: shutdown requested by signal {}", signal_number);
                aqua::log_info("client: press Ctrl+C again to force quit without cleanup");
                client.stop();
                ioc.stop();
                return;
            }
            aqua::log_error_fmt(
                "client: forced shutdown requested by signal {} (second signal, skipping cleanup)",
                signal_number);
            aqua::shutdown_logger();
            ::_Exit(128 + signal_number);
        };
        signals.async_wait(on_signal);

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

