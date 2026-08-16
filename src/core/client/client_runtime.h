#ifndef AQUA_CLIENT_RUNTIME_H
#define AQUA_CLIENT_RUNTIME_H

#include "core/diagnostics/diagnostics_manager.h"
#include "core/public/audio_format.h"
#include "core/public/config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace aqua::client {

// 客户端运行时配置。前端（CLI / UI）填充后传入 ClientRuntime::start()。
// CLI 参数中"0 = 用默认值"的语义由前端在填 runtime 前解析，core 不感知。
struct ClientConfig {
    std::string server_ip = "127.0.0.1";
    std::uint16_t server_rpc_port = 50051;
    config::RuntimeConfig runtime; // jitter 延迟 / 漂移阈值 / 播放缓冲大小
    // 断线自动重连（指数退避 1/2/4/8/16/30s），默认关闭。
    bool auto_reconnect = false;
    // gRPC Connect 时上报的名称，仅用于服务器日志识别设备，默认 "aqua_client"。
    std::string client_name = "aqua_client";
};

// 客户端运行状态。
enum class ClientState {
    Idle = 0,       // 未启动
    Connecting,     // gRPC 连接 + UDP 握手 + 播放初始化中
    Playing,        // 音频正常播放中
    Reconnecting,   // 断线后指数退避等待重连（仅 auto_reconnect 时出现）
    Stopped,        // 优雅关闭（shutdown() 或非重连模式下自然退出）
    Failed,         // 致命错误（不可恢复）
};

// 客户端事件回调。所有回调在客户端会话线程触发，不得阻塞。
struct ClientCallbacks {
    // 状态迁移（Idle → Connecting → Playing → …，含 Reconnecting/Stopped/Failed）。
    std::function<void(ClientState state)> on_state_change;
    // gRPC Connect 成功、拿到服务器固定 AudioFormat 时触发（每次重连都会触发）。
    std::function<void(AudioFormat format)> on_format;
    // 致命错误：运行时进入 Failed 并即将停止。message 为可读原因。
    std::function<void(std::string message)> on_error;
    // 优雅关闭完成（shutdown() / 致命错误 / 会话自然结束后），会话线程已 join。
    std::function<void()> on_stopped;
};

// 客户端运行时：把 gRPC Connect、UDP 握手、JitterBuffer 调度、DiagnosticsManager、
// 播放后端、HELLO 保活、断线检测、自动重连退避、优雅关闭等编排逻辑从 CLI main
// 收敛到 core。CLI / C API / 未来 UI 复用同一套生命周期。
//
// 线程模型（详见 AGENT.md §24）：
//   - 会话线程：start() 启动的后台线程，执行整个会话（含重连退避）
//   - UDP I/O 线程：asio::io_context.run()（收发 + JB 调度 + HELLO 保活）
//   - 播放线程：平台音频后端回调
//   - 调用方线程：start() / run() / shutdown()
class ClientRuntime {
public:
    ClientRuntime();
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    // 异步启动：后台会话线程执行连接、播放与监控。返回 false 表示已在运行。
    // 连接结果经回调上报（on_state_change / on_format / on_error），不阻塞调用方。
    bool start(const ClientConfig& cfg, ClientCallbacks cb = {});

    // 阻塞等待运行结束（shutdown() / stop_when 返回 true / 致命错误 / 会话自然结束）。
    // 返回前会话线程已 join，on_stopped 已触发。start() 失败时为空操作。
    void run(std::function<bool()> stop_when = {});

    // 请求优雅关闭（非阻塞，仅置位原子标志，可安全地从信号处理函数 / 其他线程调用）。
    void shutdown() noexcept;

    bool is_running() const noexcept;

    // 当前运行状态。线程安全。
    ClientState state() const noexcept;

    // 最近一次致命错误信息。按值返回，线程安全。
    std::string last_error() const;

    // 最近一次诊断快照（DiagnosticsManager 进入播放态后每 5s 刷新一次）。
    // 返回 std::nullopt 表示尚未产生快照（未启动 / 未进入播放态 / 首个周期未到 /
    // 重连后新会话尚未产出）。线程安全：返回锁内拷贝。
    std::optional<diag::DiagnosticsManager::Snapshot> diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aqua::client

#endif // AQUA_CLIENT_RUNTIME_H
