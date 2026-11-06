#include "server_session.h"

#include <asio.hpp>

#include <aqua/logger/logger.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <string>
#include <utility>

namespace aqua::gui {
namespace {

using Clock = std::chrono::steady_clock;

double delta_per_second(std::uint64_t now, std::uint64_t prev, double seconds)
{
    if (seconds <= 0.0 || now < prev) {
        return 0.0;
    }
    return static_cast<double>(now - prev) / seconds;
}

// 状态栏上"数据面到底绑在哪"的一行文本：地址 + 实际端口（不是配置端口——端口
// 传 0 时由系统分配，"0" 对排障没有意义）。多族用 / 分隔，与 client 的口径一致。
std::string build_udp_bind(const aqua::runtime::ServerRuntimeConfig& config, std::uint16_t port)
{
    const std::string port_text = std::to_string(port);
    std::string text;
    if (!config.udp_ipv4.empty()) {
        text += config.udp_ipv4 + ":" + port_text;
    }
    if (!config.udp_ipv6.empty()) {
        if (!text.empty()) {
            text += " / ";
        }
        text += "[" + config.udp_ipv6 + "]:" + port_text;
    }
    return text.empty() ? std::string("关闭") : text;
}

} // namespace

ServerSession& ServerSession::instance()
{
    static ServerSession session;
    return session;
}

ServerSession::~ServerSession()
{
    stop();
    wait_for_stop(5000);
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ServerSession::start(const aqua::runtime::ServerRuntimeConfig& config)
{
    if (active_.load(std::memory_order_acquire)) {
        return false;
    }
    // 上一轮已经结束（active_ = false）：把线程收掉再开新的。
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard lock(mutex_);
        config_ = config;
        stop_requested_ = false;
        shutdown_started_.store(false, std::memory_order_release);
        snapshot_ = ServerSnapshot { };
        snapshot_.phase = ServerPhase::Starting;
        snapshot_.status_detail = "正在启动…";
    }
    active_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&ServerSession::worker_main, this);
    } catch (const std::exception& e) {
        active_.store(false, std::memory_order_release);
        publish(ServerPhase::Failed, std::string("无法创建服务线程：") + e.what());
        return false;
    }
    return true;
}

void ServerSession::stop()
{
    std::shared_ptr<asio::io_context> ioc;
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        ioc = io_context_;
        if (snapshot_.phase == ServerPhase::Running || snapshot_.phase == ServerPhase::Starting) {
            snapshot_.phase = ServerPhase::Stopping;
            snapshot_.status_detail = "正在停止…";
            snapshot_.live = false;
        }
    }
    // 必须让 io_context **继续跑**：teardown 里 UdpTransport::stop() 是把
    // close(socket) post 到 strand 上执行的，先 ioc->stop() 会让那些任务永远不执行，
    // 端口直到进程退出都不释放——这正是"停止后再启动必报 10048"的根因。
    // post 一个任务：run() 先执行它（停 runtime），再排空关闭任务，最后无活可干返回。
    if (ioc != nullptr) {
        asio::post(*ioc, [this] { shutdown_on_io(); });
    }
}

void ServerSession::shutdown_on_io()
{
    // 幂等：stop() 的 posted 任务与控制面 tick（Degraded/Fatal）可能同时命中。
    if (shutdown_started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::shared_ptr<aqua::runtime::ServerRuntime> rt;
    std::shared_ptr<asio::steady_timer> timer;
    {
        std::lock_guard lock(mutex_);
        rt = runtime_;
        timer = control_timer_;
    }
    // 先取消控制面 tick：它每 500ms 会重新武装自己，不取消 run() 永远不空。
    if (timer != nullptr) {
        timer->cancel();
    }
    bool failed = false;
    {
        std::lock_guard lock(mutex_);
        failed = (snapshot_.phase == ServerPhase::Failed);
    }
    if (!failed) {
        publish(ServerPhase::Stopping, "正在停止…");
    }
    if (rt != nullptr) {
        rt->stop(); // 同步 teardown；socket 的 close 已 post，由 run() 继续执行
    }
}

void ServerSession::wait_for_stop(int max_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, max_ms));
    while (active_.load(std::memory_order_acquire) && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

ServerSnapshot ServerSession::poll() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void ServerSession::publish(ServerPhase phase, std::string detail)
{
    std::lock_guard lock(mutex_);
    snapshot_.phase = phase;
    snapshot_.status_detail = std::move(detail);
    snapshot_.live = (phase == ServerPhase::Running);
}

void ServerSession::publish_running(const aqua::runtime::ServerRuntimeConfig& config,
    std::uint16_t port, aqua::net::FamilySet served)
{
    std::lock_guard lock(mutex_);
    if (stop_requested_) {
        // 启动过程中用户已经点了停止：不要把界面翻回"运行中"再翻回来。
        snapshot_.phase = ServerPhase::Stopping;
        snapshot_.status_detail = "正在停止…";
        snapshot_.live = false;
        return;
    }
    snapshot_.phase = ServerPhase::Running;
    snapshot_.live = true;
    snapshot_.status_detail = "运行中";
    snapshot_.rpc_endpoint = config.rpc_ip + ":" + std::to_string(config.rpc_port);
    snapshot_.udp_bind = build_udp_bind(config, port != 0 ? port : config.udp_port);
    snapshot_.udp_port = port;
    snapshot_.served_families = served.to_string();
    snapshot_.uptime_s = 0.0;
}

void ServerSession::worker_main_impl()
{
    using aqua::runtime::RuntimeState;
    using aqua::runtime::ServerRuntime;
    // CaptureServiceAction 是 ServerRuntime 的**嵌套**枚举（不是 runtime 命名空间
    // 的成员），用别名省掉每处 ServerRuntime:: 前缀。
    using CaptureAction = ServerRuntime::CaptureServiceAction;

    aqua::runtime::ServerRuntimeConfig config;
    {
        std::lock_guard lock(mutex_);
        config = config_;
    }

    std::shared_ptr<asio::io_context> ioc;
    std::shared_ptr<ServerRuntime> runtime;
    try {
        ioc = std::make_shared<asio::io_context>();
        runtime = std::make_shared<ServerRuntime>(*ioc, config);
    } catch (const std::exception& e) {
        publish(ServerPhase::Failed, std::string("创建服务端运行时失败：") + e.what());
        active_.store(false, std::memory_order_release);
        return;
    } catch (...) {
        publish(ServerPhase::Failed, "创建服务端运行时失败（未知异常，详见运行日志）");
        active_.store(false, std::memory_order_release);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        io_context_ = ioc;
        runtime_ = runtime;
    }

    publish(ServerPhase::Starting, "正在启动采集与网络…");

    bool started = false;
    try {
        started = runtime->start();
    } catch (const std::exception& e) {
        publish(ServerPhase::Failed, std::string("启动失败：") + e.what());
    } catch (...) {
        publish(ServerPhase::Failed, "启动失败（未知异常，详见运行日志）");
    }
    if (!started) {
        runtime->stop();
        {
            std::lock_guard lock(mutex_);
            io_context_.reset();
            runtime_.reset();
            if (snapshot_.phase != ServerPhase::Failed) {
                snapshot_.phase = ServerPhase::Failed;
                snapshot_.status_detail = "启动失败：端口被占用或采集设备不可用（详见运行日志）";
                snapshot_.live = false;
            }
        }
        active_.store(false, std::memory_order_release);
        return;
    }

    const auto started_at = Clock::now();

    // start() 期间可能已经收到停止请求：此时不必进入"运行中"。
    bool abort = false;
    {
        std::lock_guard lock(mutex_);
        abort = stop_requested_;
    }
    if (!abort) {
        publish_running(config, runtime->udp_port(), runtime->udp_served_families());
    }

    // ---- 控制面 tick（与 CLI 的 control_tick 同一张决策表）----
    // capture 设备错误 / 默认设备变化 -> service_capture_switching；Fatal 或
    // Degraded 是仅有的两个终止条件（无采集的会话没有意义）。
    auto timer = std::make_shared<asio::steady_timer>(*ioc);
    std::function<void(const asio::error_code&)> tick;
    std::weak_ptr<ServerRuntime> weak_runtime = runtime;
    tick = [this, timer, &tick, weak_runtime, ioc](const asio::error_code& ec) {
        if (ec) {
            return;
        }
        const auto rt = weak_runtime.lock();
        if (!rt) {
            return;
        }
        if (rt->state() == RuntimeState::Degraded) {
            publish(ServerPhase::Failed, "运行时降级（采集或网络不可用），已停止（详见运行日志）");
            shutdown_on_io();
            return;
        }
        const auto action = rt->service_capture_switching();
        if (action == CaptureAction::Fatal) {
            publish(ServerPhase::Failed, "采集链耗尽（设备切换失败），已停止（详见运行日志）");
            shutdown_on_io();
            return;
        }
        timer->expires_after(aqua::runtime::RUNTIME_CONTROL_POLL_INTERVAL);
        timer->async_wait(tick);
    };
    {
        std::lock_guard lock(mutex_);
        control_timer_ = timer;
    }
    if (abort) {
        // start() 期间就点了停止：仍然要让 run() 跑起来执行 teardown/关闭任务。
        asio::post(*ioc, [this] { shutdown_on_io(); });
    } else {
        tick(asio::error_code { });
    }

    // ---- 诊断采集（500ms，与 client 的节奏一致）----
    // 速率是"两份快照的差 / 时间差"：core 只给累计值，速率在边界派生——与 CLI
    // 的 SnapshotView 同一条规矩，只是这里不需要跨拍状态对象。
    {
        std::jthread diag_thread([this, weak_runtime, started_at](std::stop_token st) {
            // **线程函数内也必须兜异常**：take_diagnostics_snapshot /
            // sessions_snapshot / 字符串拼接 / vector 扩容都可能抛，jthread 不
            // 捕获，一旦逃出 lambda 就是 std::terminate（整个进程消失）。
            try {
            aqua::diagnostics::ServerDiagnosticsSnapshot prev { };
            Clock::time_point prev_at { };
            bool have_prev = false;
            while (!st.stop_requested()) {
                const auto rt = weak_runtime.lock();
                if (!rt) {
                    break;
                }
                const auto current = rt->take_diagnostics_snapshot();
                const auto now = Clock::now();
                // 连接列表：SessionManager 的一致性快照 → 纯显示行（时间点在这里
                // 换算成"距今秒数"，UI 不做时间运算）。session 数是个位数量级。
                std::vector<SessionRow> session_rows;
                {
                    const auto sessions = rt->sessions_snapshot();
                    session_rows.reserve(sessions.size());
                    for (const auto& info : sessions) {
                        SessionRow row;
                        row.id = info.session_id;
                        // 客户端自报的名字（Connect 里带上来）；空 = 未上报。
                        row.name = info.name;
                        row.connected = (info.state
                            == aqua::session::SessionManager::SessionState::Connected);
                        if (info.family == aqua::net::IpFamily::V4) {
                            row.family = 4;
                        } else if (info.family == aqua::net::IpFamily::V6) {
                            row.family = 6;
                        }
                        if (info.endpoint.port() != 0) {
                            const std::string address = info.endpoint.address().to_string();
                            row.endpoint = (row.family == 6)
                                ? "[" + address + "]:" + std::to_string(info.endpoint.port())
                                : address + ":" + std::to_string(info.endpoint.port());
                        }
                        row.age_s = std::chrono::duration<double>(now - info.created_at).count();
                        session_rows.push_back(std::move(row));
                    }
                }
                ServerRates rates { };
                if (have_prev) {
                    const double seconds = std::chrono::duration<double>(now - prev_at).count();
                    rates.capture_fps = delta_per_second(current.capture.captured_frames,
                        prev.capture.captured_frames, seconds);
                    rates.encoded_fps = delta_per_second(current.dispatcher.frames_encoded,
                        prev.dispatcher.frames_encoded, seconds);
                    rates.broadcast_fps = delta_per_second(current.dispatcher.frames_broadcast,
                        prev.dispatcher.frames_broadcast, seconds);
                    rates.no_client_fps = delta_per_second(current.dispatcher.frames_without_clients,
                        prev.dispatcher.frames_without_clients, seconds);
                    rates.rx_pps_v4 = delta_per_second(current.net.transport.v4.rx_packets,
                        prev.net.transport.v4.rx_packets, seconds);
                    rates.tx_pps_v4 = delta_per_second(current.net.transport.v4.tx_packets,
                        prev.net.transport.v4.tx_packets, seconds);
                    rates.rx_pps_v6 = delta_per_second(current.net.transport.v6.rx_packets,
                        prev.net.transport.v6.rx_packets, seconds);
                    rates.tx_pps_v6 = delta_per_second(current.net.transport.v6.tx_packets,
                        prev.net.transport.v6.tx_packets, seconds);
                    // 两族合计的发送字节速率（tx_bytes 是累计值，两族相加后再差分）。
                    rates.tx_bytes_per_s = delta_per_second(
                        current.net.transport.v4.tx_bytes + current.net.transport.v6.tx_bytes,
                        prev.net.transport.v4.tx_bytes + prev.net.transport.v6.tx_bytes,
                        seconds);
                    rates.heartbeat_ps = delta_per_second(current.net.heartbeat_received,
                        prev.net.heartbeat_received, seconds);
                    rates.queue_drop_ps = delta_per_second(current.queue.dropped_frames,
                        prev.queue.dropped_frames, seconds);
                }
                {
                    std::lock_guard lock(mutex_);
                    if (snapshot_.phase == ServerPhase::Running
                        || snapshot_.phase == ServerPhase::Stopping) {
                        snapshot_.diag = current;
                        snapshot_.rates = rates;
                        snapshot_.sessions = session_rows;
                        snapshot_.uptime_s = std::chrono::duration<double>(now - started_at).count();
                    }
                }
                prev = current;
                prev_at = now;
                have_prev = true;
                // 分片睡眠：停止请求至多晚 50ms 被观察到。
                // 10 × 50 = 500ms 一拍：与 client 的诊断节奏对齐（两端同一份读数
                // 手感，速率也更稳——250ms 的差分对调度抖动太敏感）。
                for (int i = 0; i < 10 && !st.stop_requested(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            } catch (const std::exception& e) {
                aqua::log_error(std::string("server: diagnostics thread aborted: ") + e.what());
            } catch (...) {
                aqua::log_error("server: diagnostics thread aborted: unknown exception");
            }
        });

        ioc->run(); // stop() -> ioc->stop() / 控制面终止条件都会让它返回

        // 显式收掉诊断线程再 teardown：runtime->stop() 期间不应再有人读它的统计。
        diag_thread.request_stop();
    } // diag_thread 在这里 join

    // run() 正常返回的唯一途径就是"关闭任务已排空"；这里再调一次是幂等的兜底
    // （覆盖 start 失败等没有 tick 的路径）。
    shutdown_on_io();

    bool failed = false;
    {
        std::lock_guard lock(mutex_);
        failed = (snapshot_.phase == ServerPhase::Failed);
    }
    if (!failed) {
        publish(ServerPhase::Stopping, "正在停止…");
    }
    runtime->stop();

    {
        std::lock_guard lock(mutex_);
        io_context_.reset();
        runtime_.reset();
        control_timer_.reset();
        snapshot_.live = false;
        snapshot_.sessions.clear();
        snapshot_.rates = ServerRates { };
        snapshot_.uptime_s = 0.0;
        if (!failed) {
            snapshot_.phase = ServerPhase::Idle;
            snapshot_.status_detail = "已停止";
        }
        snapshot_.rpc_endpoint.clear();
        snapshot_.udp_bind.clear();
        snapshot_.served_families.clear();
        snapshot_.udp_port = 0;
    }
    active_.store(false, std::memory_order_release);
}

void ServerSession::worker_main()
{
    // **异常绝不能逃出线程函数**：std::thread 里未捕获的异常 = std::terminate。
    // impl 里除了已包 try 的构造 / start，还有 tick 的首次调用、ioc->run() 里
    // 的 handler、诊断采集、字符串拼接、teardown —— 任何一处 bad_alloc /
    // asio system_error 都会冒到这里。client_session::worker_main 同款边界。
    try {
        worker_main_impl();
    } catch (const std::exception& e) {
        fail_worker(std::string("服务线程异常终止：") + e.what());
    } catch (...) {
        fail_worker("服务线程异常终止（未知原因，详见运行日志）");
    }
}

void ServerSession::fail_worker(std::string reason)
{
    try {
        aqua::log_error("server: " + reason);
    } catch (...) {
        // 收尾路径上日志本身再抛异常也不能越过线程边界。
    }
    std::shared_ptr<asio::io_context> ioc;
    std::shared_ptr<aqua::runtime::ServerRuntime> rt;
    std::shared_ptr<asio::steady_timer> timer;
    {
        std::lock_guard lock(mutex_);
        ioc = io_context_;
        rt = runtime_;
        timer = control_timer_;
        io_context_.reset();
        runtime_.reset();
        control_timer_.reset();
        snapshot_.phase = ServerPhase::Failed;
        snapshot_.status_detail = std::move(reason);
        snapshot_.live = false;
        snapshot_.sessions.clear();
        snapshot_.rates = ServerRates { };
        snapshot_.uptime_s = 0.0;
        // 与正常 teardown（worker_main_impl 末尾）一样清掉"这一场运行"的端点：
        // 状态栏的「· 控制 x.x.x.x:port」只按 phase 拼接、没有 live 门控，
        // 不清就会在异常终止后一直挂着上一场的控制端点。
        snapshot_.rpc_endpoint.clear();
        snapshot_.udp_bind.clear();
        snapshot_.served_families.clear();
        snapshot_.udp_port = 0;
    }
    // 控制面 tick 每拍都会重新武装自己：先 cancel，否则下面的排空 run() 不会返回。
    if (timer != nullptr) {
        try {
            timer->cancel();
        } catch (...) {
        }
    }
    if (rt != nullptr) {
        try {
            rt->stop();
        } catch (...) {
        }
    }
    // UdpTransport::stop() 把 close(socket) post 到 strand 上；必须让 io_context
    // 再跑一轮把这些任务排空，否则端口直到进程退出都不释放（10048）。run() 若
    // 因 handler 抛异常退出，restart() 之后才能再次调用。
    if (ioc != nullptr) {
        try {
            // 只排空**已经就绪**的收尾任务（socket close 是 posted 的），不阻塞
            // 等待。用 run() 的话，只要还有任何持续异步工作，就会永久卡在这里，
            // active_ 不复位、界面卡在"停止中"且无法重启。250ms 足够排空 close；
            // 超时就放弃，让析构兜底（宁可端口短暂占用，也不能让 UI 卡死）。
            const auto deadline = Clock::now() + std::chrono::milliseconds(250);
            while (Clock::now() < deadline) {
                ioc->restart();
                if (ioc->poll() == 0) {
                    break;
                }
            }
        } catch (...) {
        }
    }
    // 必须有：否则 active_ 永久为 true，start() 从此一直返回 false（界面卡死）。
    active_.store(false, std::memory_order_release);
}

} // namespace aqua::gui
