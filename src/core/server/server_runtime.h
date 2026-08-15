#ifndef AQUA_SERVER_RUNTIME_H
#define AQUA_SERVER_RUNTIME_H

#include "core/public/audio_format.h"
#include "core/public/config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aqua::server {

// 服务器运行时配置。前端（CLI / UI）填充后传入 ServerRuntime::start()。
// 与 CLI 参数一一对应；core 不依赖 cxxopts，由前端完成解析。
struct ServerConfig {
    std::string bind_ip = "0.0.0.0";
    std::uint16_t rpc_port = 50051;
    std::uint16_t udp_port = 50000;
    config::RuntimeConfig runtime; // 采集 RingBuffer 大小等可调参数
};

// 服务器事件回调。所有回调在服务器内部线程触发，不得阻塞；重活投递到调用方线程。
struct ServerCallbacks {
    // 全部子系统启动完成（采集 / gRPC / UDP / packetizer / 清理定时器）。
    std::function<void()> on_started;
    // 致命错误：运行时已进入关闭流程，on_stopped 即将触发。message 为可读原因。
    std::function<void(std::string message)> on_error;
    // 优雅关闭完成（shutdown() 或致命错误触发，线程已 join）。
    std::function<void()> on_stopped;
};

// 服务器运行时：把音频采集、SessionManager、gRPC 控制面、UDP 数据面、
// packetizer、session 超时清理、健康监控、优雅关闭等组件的编排逻辑从 CLI main
// 收敛到 core。CLI / C API / 未来 UI 复用同一套生命周期，避免每个前端重写编排。
//
// 线程模型（详见 AGENT.md §24）：
//   - gRPC 线程：GrpcServer::run()
//   - UDP I/O 线程：asio::io_context.run()（收发 + session 清理定时器）
//   - packetizer 线程：RingBuffer → 编码 → 广播
//   - 采集线程：平台音频后端回调
//   - 调用方线程：start() / run() / shutdown()
class ServerRuntime {
public:
    ServerRuntime();
    ~ServerRuntime();

    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    // 同步启动全部子系统并返回。成功时所有线程已启动、on_started 已触发。
    // 失败返回 false（last_error() 可查原因），且已启动的子系统已回滚清理。
    bool start(const ServerConfig& cfg, ServerCallbacks cb = {});

    // 阻塞运行健康监控主循环，直到 shutdown() 被调用、stop_when 返回 true
    // 或检测到致命错误（采集/gRPC 后端异常退出）。返回前完成全部资源清理，
    // 返回后 on_stopped 已触发。start() 失败时调用本函数为空操作。
    void run(std::function<bool()> stop_when = {});

    // 请求优雅关闭（非阻塞，仅置位原子标志，可安全地从信号处理函数 / 其他线程调用）。
    // 实际的停止与线程 join 在 run() 循环内完成。
    void shutdown() noexcept;

    // 是否已成功启动且尚未停止。
    bool is_running() const noexcept;

    // 最近一次错误信息（start() 失败或 on_error 触发时）。按值返回，线程安全。
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aqua::server

#endif // AQUA_SERVER_RUNTIME_H
