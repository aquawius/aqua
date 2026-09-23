// aqua_server_cli：完整 server。参数解析在 cli_parser_server，装配与生命周期在 ServerRuntime。

#include "aqua/diagnostics/diagnostics_config.h"
#include "aqua/diagnostics/diag_view.h"
#include "aqua/diagnostics/diagnostics.h"
#include "aqua/logger/logger.h"
#include "aqua/net/address/address_utils.h"
#include "aqua/runtime/server_runtime.h"

#include "cli_parser/cli_parser_server.h"

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
    // signal_thread / runtime）join 之后最后执行同步排空，保证关机尾部不断行。
    // 第二次信号的 ::_Exit 强制路径不经过这里（by design：不做清理，
    // 但强退分支里会显式排空一次）。
    struct LogDrain {
        ~LogDrain() { aqua::shutdown_logger(); }
    };
    const LogDrain log_drain;
    aqua::cli::configure_console_utf8();
    aqua::runtime::ServerRuntimeConfig cfg;
    aqua::LogLevel log_level = aqua::default_log_level();
    std::string log_file; // --log-file：日志 tee 到文件（见 aqua::init_logger）
    if (const auto exit_code = aqua::cli::cli_exit_code(
            aqua::cli::parse_server_cli(argc, argv, cfg, log_level, log_file))) {
        return *exit_code;
    }

    try {
        aqua::init_logger(log_file);
        aqua::set_log_level(log_level);
        aqua::log_debug_fmt(
            "CLI config: log_level={} server_ip={} rpc_port={} udp_port={} advertise={} format={}ch/{}Hz/enc={} packet_frames={} queue_capacity={} capture_source={} capture_device={}",
            aqua::log_level_name(log_level), cfg.server_ip, cfg.rpc_port, cfg.udp_port,
            cfg.advertised_udp_address.empty() ? cfg.server_ip : cfg.advertised_udp_address,
            cfg.format ? cfg.format->channels : 0, cfg.format ? cfg.format->sample_rate : 0,
            cfg.format ? static_cast<int>(cfg.format->encoding) : static_cast<int>(aqua::audio::AudioEncoding::INVALID),
            cfg.frame_count, cfg.audio_queue_capacity_slots,
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
        // 默认回落 advertised=server_ip：bind 通配符（默认 0.0.0.0）时，下发的通告
        // 也是通配符，client 会回落用 gRPC 地址拨号（grpc_client fallback），单网卡
        // 场景可工作。多网卡/NAT 下回落地址未必可达，显式 --udp-advertise-ip 才可靠。
        try {
            if (cfg.advertised_udp_address.empty()
                && aqua::net::parse_ip_address(cfg.server_ip).is_unspecified()) {
                aqua::log_info_fmt(
                    "server: bind address {} is a wildcard; clients fall back to the gRPC address "
                    "for UDP (set --udp-advertise-ip on multi-homed/NAT hosts)",
                    cfg.server_ip);
            }
        } catch (...) {
            // server_ip 已在 parser 校验过字面量；此处解析失败不影响启动，仅跳过提示。
        }
        aqua::log_info_fmt("server: bind {} gRPC:{} udp:{} advertise={} ({}ch/{}Hz/enc={}, F={})",
            cfg.server_ip, cfg.rpc_port, server->udp_port(),
            aqua::net::format_host_port(advertised_udp_ip, advertised_udp_port),
            server->audio_format().channels, server->audio_format().sample_rate,
            static_cast<int>(server->audio_format().encoding), server->frame_count());
        // 诊断源统一读聚合快照（aqua::diagnostics::ServerDiagnosticsSnapshot）：diag tick
        // 先刷新一次，保证同一行内各模块块来自同一份近似读值。每个模块由 ServerDiagView
        // 渲染成一段紧凑块（audio{...} capture{...} pktz{...} queue{...} dsp{...} net{...}
        // sess{...}）；模块内速率以 T/D/R 缩写（total / 距上次快照增量 / 每秒速率），详见
        // aqua_core/doc/diagnostics.md。tick 与 Diagnostics 求值在专用 diag 线程上顺序执行（不得占用
        // 网络 ioc，见下方 diag_thread 注释），快照内只有该线程一个读写者。
        auto snapshot = std::make_shared<aqua::diagnostics::ServerDiagnosticsSnapshot>(
            server->take_diagnostics_snapshot());
        aqua::diagnostics::ServerDiagView diag_view(cfg.capture.source);
        aqua::diagnostics::Diagnostics diag("Server");
        diag.add_source("state", [&] { return diag_view.render_state(*snapshot, server->udp_port()); });
        diag.add_source("audio", [&] { return diag_view.render_audio(*snapshot); });
        diag.add_source("capture", [&] { return diag_view.render_capture(*snapshot); });
        diag.add_source("pktz", [&] { return diag_view.render_packetizer(*snapshot); });
        diag.add_source("queue", [&] { return diag_view.render_queue(*snapshot); });
        diag.add_source("dsp", [&] { return diag_view.render_dispatcher(*snapshot); });
        diag.add_source("net", [&] { return diag_view.render_net(*snapshot); });
        diag.add_source("sess", [&] { return diag_view.render_sessions(*snapshot); });
        // 诊断 tick 用独立线程而非 ioc 定时器：即便诊断行已压缩成紧凑块，同步写
        // 控制台（Windows 控制台写可阻塞数 ms ~ 数十 ms）仍会卡住 tx strand 发包泵
        // 与 session/heartbeat 处理，在链路上制造周期性收包空隙。独立线程承担格式化
        // 与写盘，避免该问题。take_diagnostics_snapshot 是 C API 契约、任意线程可调，
        // 无并发问题。
        // std::jthread：析构时自动 request_stop + join，取代原先的
        // atomic<bool> 停止旗 + 手写 DiagThreadJoin RAII。
        // 声明顺序保证 diag_thread 先于其引用的 server / snapshot / diag 析构。
        std::jthread diag_thread([&](std::stop_token st) {
            while (!st.stop_requested()) {
                *snapshot = server->take_diagnostics_snapshot();
                diag.log_debug();
                // 分片睡眠：停止请求至多晚一个分片（50ms）被观察到。
                for (int i = 0; i < 20 && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(
                        aqua::config::DIAGNOSTICS_SNAPSHOT_INTERVAL / 20);
                }
            }
        });
        aqua::log_debug("server: diagnostics snapshot interval=1000ms (dedicated thread)");

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
        asio::io_context signal_ioc;
        asio::signal_set signals(signal_ioc, SIGINT, SIGTERM, SIGBREAK);
#else
        asio::io_context signal_ioc;
        asio::signal_set signals(signal_ioc, SIGINT, SIGTERM);
#endif
        // 两段式关闭：第一次信号优雅停止（通知订阅者 + 完整 teardown），
        // 第二次信号强制退出（_Exit，不做清理——优雅停止卡住时的逃生舱）。
        // 注意：必须用独立的 signal_ioc 线程收信号；若复用主 ioc，第一次
        // stop() 阻塞期间第二个信号永远得不到派发，强制退出也就永远触发不了。
        static std::atomic<int> signal_count { 0 };
        std::function<void(const asio::error_code&, int)> on_signal;
        on_signal = [&](const asio::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }
            // 单次触发语义：先重挂，保证优雅停止卡住时第二个信号仍能送达
            // （不重挂则强制退出路径永远不可达）。
            signals.async_wait(on_signal);
            const int n = signal_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 1) {
                aqua::log_info_fmt("server: graceful shutdown requested by signal {}", signal_number);
                aqua::log_info("server: press Ctrl+C again to force quit without cleanup");
                server->stop();
                ioc.stop();
                return;
            }
            aqua::log_error_fmt(
                "server: forced shutdown requested by signal {} (second signal, skipping cleanup)",
                signal_number);
            // 强退前同步排空日志：否则正写到一半被杀，关机尾部断行。
            // 排空只等本地 sink 写完，不做任何 join/cleanup，
            // force-quit 语义不变。
            aqua::shutdown_logger();
            // 全局 ::_Exit（C99 + MSVC 均有；比 std::_Exit 可移植性更稳）：
            // 立即终止进程，不跑析构/OS 句柄由系统回收。
            ::_Exit(128 + signal_number);
        };
        signals.async_wait(on_signal);

        // std::jthread + stop_callback：stop 请求自动 signal_ioc.stop()，
        // 析构自动 join（signal_ioc 声明在线程之前，析构顺序安全）。
        std::jthread signal_thread([&](std::stop_token st) {
            std::stop_callback cb(st, [&] { signal_ioc.stop(); });
            signal_ioc.run();
        });

        ioc.run();

        server->stop();
        // 主 ioc 已停：停掉信号循环（优雅路径必达；强制路径 _Exit 不经过这里）。
        // signal_thread 的 join 由 jthread 析构完成。
        signal_ioc.stop();

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
