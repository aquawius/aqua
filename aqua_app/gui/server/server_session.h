#ifndef AQUA_APP_GUI_SERVER_SESSION_H
#define AQUA_APP_GUI_SERVER_SESSION_H

// 服务端会话层：GUI 与 aqua_server_core 的唯一接缝。
//
// 线程模型（与 client_session 同一套约定：GUI 线程永不阻塞）：
//   UI 线程     —— start() / stop() / poll()，全部立即返回；
//   worker 线程 —— 持有 io_context + ServerRuntime，跑 start()（会阻塞在采集
//                  启动与端口绑定上）、控制面 tick（capture 切换 / 降级停机）、
//                  收尾时 runtime->stop()；
//   diag 线程   —— 每 500ms 采一份 ServerDiagnosticsSnapshot 并派生速率。
//   两侧只通过互斥保护的 ServerSnapshot 交换数据（值拷贝，无共享）。
//
// 为什么快照不直接在 UI 线程取：take_diagnostics_snapshot() 会读 SessionManager
// 的统计（带锁）并拼字符串；60fps 的 UI 线程不该按帧去碰这些。反过来 500ms 足以
// 让仪表盘看起来是实时的（人的读数节奏远慢于 2Hz），也与 client 的节奏一致。
//
// 生命周期：ServerRuntime 的 start/stop 是一次性的，但本类允许"停掉再启动"——
// 每次 start() 新建一个 ServerRuntime（配置沿用启动参数）。

#include <aqua/diagnostics/server_diagnostics_snapshot.h>
#include <aqua/net/address/ip_family.h>
#include <aqua/runtime/server_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <stop_token>

namespace asio {
class io_context;
}

namespace aqua::runtime {
class ServerRuntime;
}

namespace aqua::gui {

enum class ServerPhase {
    Idle,     // 未启动 / 已停止
    Starting, // 采集与网络正在启动
    Running,
    Stopping,
    Failed, // 启动失败 / 运行时降级终止（status_detail 里是原因）
};

// 两次快照之间的派生速率（UI 只显示，不自己算）。
struct ServerRates {
    double capture_fps = 0.0;   // 采集帧率（帧/秒，应≈采样率）
    double encoded_fps = 0.0;   // 编码帧率
    double broadcast_fps = 0.0; // 广播帧率
    double no_client_fps = 0.0; // 无客户端时被跳过的帧率
    double rx_pps_v4 = 0.0;     // 数据面收包（包/秒）
    double tx_pps_v4 = 0.0;
    double rx_pps_v6 = 0.0;
    double tx_pps_v6 = 0.0;
    double heartbeat_ps = 0.0;  // 心跳到达速率
    double queue_drop_ps = 0.0; // 采集→网络队列丢帧速率
    // 发出字节速率（两族合计，字节/秒）。包速率回答"发得勤不勤"，字节速率回答
    // "占多少带宽"——运行信息卡的「分发」行两个都要。
    double tx_bytes_per_s = 0.0;

    [[nodiscard]] double tx_pps_total() const noexcept { return tx_pps_v4 + tx_pps_v6; }
};

// 一个已连接（或正在等首包）的客户端。由 SessionManager::snapshot_sessions() 在
// 诊断线程上转换而来：时间点在这里换算成"距今多少秒"，UI 只负责显示。
struct SessionRow {
    std::uint32_t id = 0;      // 会话号（与 core 日志里的 session=0x… 对齐）
    std::string endpoint;      // "1.2.3.4:5678"；未收到首包时为空
    int family = 0;            // 0=未知（还没收到 UDP 首包） 4 / 6
    bool connected = false;    // false = Created（等首包）
    double age_s = 0.0;        // 存活时长（距 Connect）
    double idle_s = 0.0;       // 距最近一次控制面 keepalive
};

// UI 线程读到的服务端快照（每次 poll 拿一份值拷贝）。
struct ServerSnapshot {
    ServerPhase phase = ServerPhase::Idle;
    std::string status_detail; // 人话（"运行中" / "启动失败：…"）
    bool live = false;         // 有正在跑的运行时（phase == Running）

    aqua::diagnostics::ServerDiagnosticsSnapshot diag {};
    ServerRates rates {};

    // 启动时确定的固定信息（停止后清空，UI 不显示上一轮的残留）
    std::string rpc_endpoint;    // "0.0.0.0:50051"
    std::string udp_bind;        // "0.0.0.0:50000 / [::]:50000"（实际绑定地址+端口）
    std::string served_families; // "v4+v6"
    std::uint16_t udp_port = 0;  // **实际**绑定端口（可能是 0 → 临时端口）

    double uptime_s = 0.0; // 本次运行时长

    // 存活 session 列表（诊断线程每 500ms 采一次；UI 直接按行渲染）。
    std::vector<SessionRow> sessions;
};

class ServerSession {
public:
    static ServerSession& instance();

    // 异步启动。已在运行（或正在启动）时返回 false。
    // 上一轮已结束时允许再次调用（会 join 旧线程后新建运行时）。
    bool start(const aqua::runtime::ServerRuntimeConfig& config);

    // 异步停止：向 worker 的 io_context post 一个 teardown 任务，立即返回。
    //
    // ⚠️ **不能**用 ioc->stop() 来代替：UdpTransport::stop() 是把 close(socket)
    // post 到 strand 上执行的，先停 io_context 会让这些任务永远不执行 ⇒ 端口直到
    // 进程退出都不释放 ⇒ 点"停止"再"启动"必然 bind 失败（WSAEADDRINUSE 10048）。
    // 所以停止请求必须让 io_context 继续跑，把关闭任务和收尾都排空后 run() 自然返回。
    void stop();

    [[nodiscard]] bool active() const { return active_.load(std::memory_order_acquire); }

    // 等 worker 收尾（退出时用；毫秒上限）。
    void wait_for_stop(int max_ms);

    // 最近一份快照（值拷贝，互斥保护）。
    [[nodiscard]] ServerSnapshot poll() const;

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

private:
    ServerSession() = default;
    ~ServerSession();

    void worker_main();
    // teardown 的唯一入口（**必须在 io_context 线程上执行**：cancel timer + 停 runtime，
    // 让 posted 的 socket 关闭任务排在它后面被 run() 执行）。
    void shutdown_on_io();
    void publish(ServerPhase phase, std::string detail);
    void publish_running(const aqua::runtime::ServerRuntimeConfig& config, std::uint16_t port,
        aqua::net::FamilySet served);

    mutable std::mutex mutex_;
    ServerSnapshot snapshot_ {};
    aqua::runtime::ServerRuntimeConfig config_ {};
    std::shared_ptr<asio::io_context> io_context_;
    std::shared_ptr<aqua::runtime::ServerRuntime> runtime_;
    std::shared_ptr<asio::steady_timer> control_timer_;
    bool stop_requested_ = false;
    // teardown 只做一次（stop() 的 posted 任务与控制面 tick 可能同时命中）。
    std::atomic<bool> shutdown_started_ { false };

    std::thread worker_;
    std::atomic<bool> active_ { false };
};

} // namespace aqua::gui

#endif // AQUA_APP_GUI_SERVER_SESSION_H
