#ifndef AQUA_APP_GUI_CLIENT_SESSION_H
#define AQUA_APP_GUI_CLIENT_SESSION_H

// 会话层：GUI 与 aqua_client_core 的唯一接缝。
//
// 线程模型（GUI 线程永不阻塞）：
//   UI 线程     —— connect() / disconnect() / poll()，全部立即返回；
//   worker 线程 —— 持有 io_context + ClientRuntime，跑 start()（会阻塞在 gRPC
//                  握手上）、周期 poll_control、发布诊断快照；
//   两侧只通过本类里互斥保护的 SessionSnapshot 交换数据（值拷贝，无共享）。
//
// 自动重连（UiState::auto_reconnect）也在这里：会话非用户主动终止后等 3 秒
// 重建 ClientRuntime（与 Android 策略一致）；连接根本没建立起来（start 失败）
// 不重试——那是配置/服务器不可达，重试只会让用户莫名等待。
//
// 日志：core 的 spdlog 输出由 log_sink 转发进运行日志面板，本类只补充少量
// 用户视角的状态行（"正在连接 1.2.3.4:50051" 等）。

#include "jb_presets.h"
#include "ui_state.h"

#include <aqua/audio/devices/audio_device.h>
#include <aqua/diagnostics/client_diagnostics_snapshot.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace asio {
class io_context;
}

namespace aqua::runtime {
class ClientRuntime;
}

namespace aqua::gui {

// 一次连接的配置（UI 线程从 UiState 组装，值语义，交给 worker）。
struct SessionConfig {
    std::string rpc_ip;
    std::uint16_t rpc_port = 0;
    std::optional<std::uint16_t> udp_force_port; // 空 = 采用 Server 通告
    bool pin_device = false;
    aqua::audio::AudioDeviceId device_id; // 空 = 跟随系统默认
    bool auto_reconnect = false;
    int reconnect_delay_s = 3; // 断线后等待多少秒再重建（设置 → 连接，1–10 秒）
    JbParams jb; // 已按预设解析好的 6 个参数
};

// UI 线程读到的会话快照（每次 poll 拿一份值拷贝）。
struct SessionSnapshot {
    UiPhase phase = UiPhase::Idle;
    std::string status_detail; // 最近一次状态变化 / 错误（人话，展示 + 入日志）
    bool has_diagnostics = false;
    aqua::diagnostics::ClientDiagnosticsSnapshot diagnostics {};
    int reconnect_attempt = 0; // >0 = 正在第 N 次自动重连（UI 可显示倒计时语义）

    // ---- 会话建立时确定的固定信息（连接成功那一次写，**离开 Connected 时清空**）----
    // 遥测面板直接渲染这几个字段；status_detail 里也有一份，但那是给人和日志看的
    // 一句话，不该被 UI 反解析。清空是硬要求：断开后没有"本次会话"，UI 必须显示
    // 占位而不是上一场会话的格式串（用户 2026-10-18 反馈）。
    std::uint64_t session_id = 0;
    std::string data_plane;  // "ip:port"；空 = 服务端未通告 / 未知
    // 一行摘要，对齐 Android 的「音频格式」卡：采样率 / 声道 / 编码 / 码率 / 帧长。
    std::string audio_format;
    int sample_rate = 0;     // 0 = 未通告
    int channels = 0;
    // 分项：设置→统计要逐项列（采样率 / 声道 / 编码 / 位深 / 码率 / 帧长），
    // 在这里拆好，UI 不再反解析上面那行摘要。
    std::string encoding;    // "PCM F32LE"
    int bits_per_sample = 0; // 32
    int bitrate_kbps = 0;    // 3072
    int frame_count = 0;     // 每包帧数 F
    double packet_ms = 0.0;  // 包时长
    // 每发布一次自增。UI 侧的趋势缓冲靠它去重——每帧 poll 拿到的是同一份快照，
    // 不能按帧采样（否则 60fps 下 60 秒历史两秒就填满）。
    std::uint64_t snapshot_seq = 0;
    // UDP 音频包到达速率（包/秒）：core 只给累计收包数，速率在这里按两次快照的
    // 差值 / 实际间隔算好（EMA 平滑），UI 不再自己跨帧推算。
    double rx_rate = 0.0;
};

class ClientSession {
public:
    ClientSession();
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // 异步连接：立即返回。false = 已有会话在跑（调用方应忽略本次点击）。
    bool connect(const SessionConfig& config);
    // 异步断开：立即返回；worker 收尾后阶段变 Idle（poll 可见）。
    void disconnect();
    // UI 每帧调用：最新快照（互斥拷贝，不阻塞）。
    [[nodiscard]] SessionSnapshot poll() const;
    // worker 是否还在跑（断开中为 true）。
    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_acquire); }

private:
    void worker_main(SessionConfig config);
    void publish(UiPhase phase, std::string detail);
    void publish_diagnostics(const aqua::diagnostics::ClientDiagnosticsSnapshot& snapshot,
        int reconnect_attempt);
    void clear_runtime_handles();

    mutable std::mutex mutex_;
    SessionSnapshot snapshot_;
    // worker 发布、UI 线程 stop 用的句柄。shared_ptr 保证 disconnect() 与
    // worker 收尾并发时对象不会被就地析构（core 的 stop() 声明可跨线程调用）。
    std::shared_ptr<aqua::runtime::ClientRuntime> runtime_;
    std::shared_ptr<asio::io_context> io_context_;

    std::thread worker_;
    std::atomic<bool> active_ { false };
    std::atomic<bool> stop_requested_ { false };
    // worker 是否正在 start()（阻塞在 gRPC 握手）：此时 disconnect() 不能调
    // runtime->stop()（会等 start() 让出 lifecycle_mutex_，UI 就冻了）。
    std::atomic<bool> handshaking_ { false };
    // 到达速率的基线（只在 publish_diagnostics 里用，worker 线程独占）。
    std::uint64_t rx_rate_last_packets_ = 0;
    std::chrono::steady_clock::time_point rx_rate_last_at_ {};
    bool rx_rate_has_base_ = false;
    double rx_rate_ema_ = 0.0;
};

// 进程级单例（与 log_buffer() 同构；main.cpp 与面板共用同一个会话）。
ClientSession& client_session();

// 把快照写进 UiState（主循环每帧调）：阶段 + 「会话数据」视图。
// 指标映射：水位/领先/目标/抖动/缺口率/欠载，口径与 ClientDiagnosticsSnapshot 一致。
void apply_session_snapshot(const SessionSnapshot& snapshot, UiState& state);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_CLIENT_SESSION_H
