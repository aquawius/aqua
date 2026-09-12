#ifndef AQUA_RUNTIME_CLIENT_RUNTIME_H
#define AQUA_RUNTIME_CLIENT_RUNTIME_H

// ClientRuntime：client 侧的统领（唯一入口）。
//
// 生命周期是一次性的：Created -> Starting -> Running/Degraded -> Stopping -> Stopped。
// start() / stop() 内部串行化；stop() 可安全地从其它控制线程并发调用，
// 但会等待当前 start() 完成后再执行 teardown。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/buffer/jitter_buffer.h"
#include "aqua/audio/buffer/jitter_estimator.h"
#include "aqua/audio/buffer/target_controller.h"
#include "aqua/audio/devices/audio_device_manager.h"
#include "aqua/audio/playback/audio_playback_config.h"
#include "aqua/audio/playback/playback_manager.h"
#include "aqua/diagnostics/client_diagnostics_snapshot.h"
#include "aqua/logger/logger.h"
#include "aqua/net/grpc/grpc_client.h"
#include "aqua/net/udp/udp_client.h"
#include "aqua/net/udp/udp_config.h"
#include "aqua/runtime/runtime_config.h"
#include "aqua/runtime/runtime_state.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aqua::runtime {

// 回放设备在 start() 时按 playback.device 起步解析；运行期切换经
// set_playback_device() 走 PlaybackManager 事务链（playback_switching_design.md）。
struct ClientRuntimeConfig {
    std::uint32_t jb_capacity_slots = config::DEFAULT_CLIENT_JB_CAPACITY_SLOTS;
    std::chrono::milliseconds heartbeat_handshake_interval { aqua::config::HEARTBEAT_HANDSHAKE_INTERVAL };
    audio::AudioPlaybackConfig playback;
    // Phase 1 自适应 target（默认开）：开 = JB 起步用小水位 + TargetController
    // 按到达抖动动态调 target；关 = 既有固定 target/startup（0.60/0.50）。
    // 连接属性（JB 构造时确定），运行期不可切换。
    bool jb_adaptive_target = true;
    // ---- 自适应 target 的现场调优旋钮（仅 jb_adaptive_target 开时生效）----
    // 仍未暴露的：起步 target / 回落限速 / 涨后锁跌 / 死区 / 惩罚累计上限与回落
    // 速率 / concealment 连续上限——它们只有一个很窄的合理区间，暴露出去只会
    // 制造误调，要改直接改 buffer_config.h 常量重新编译（候选清单见
    // configuration_reference.md）。下面 6 个则是"现场一定会想动"的：前两个是
    // 主力旋钮，后四个是分离"预测项 k×J / 峰值项 / 反馈项 penalty"各自贡献、
    // 以及做极值实验的对照点。
    // k：margin = k×J（包）—— 延迟 ↔ 稳定的主力旋钮。取值理由与"调多大都会被
    // 2/3 结构上限接住"见 config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN。
    double jb_jitter_gain = config::JB_ADAPTIVE_DEFAULT_JITTER_GAIN;
    // target 硬下限（槽）。有效下限 = max(本值, 几何地板 + 1)：几何地板无条件
    // 托底，本旋钮只用于**抬高**最低延迟，压不到地板以下。语义见
    // audio::TargetControllerParams::min_target_slots。
    std::uint32_t jb_min_target_slots = config::JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS;
    // stall 峰值项的上限（槽）= `--jb-stall-peak-cap`。stall 是"已经发生的恢复
    // 风险信号"，不是新的 steady-state 延迟要求；本值决定一次孤立大 stall 最多
    // 把 target 推多高。**0 = 关闭 stall 峰值项**（margin 退回纯 k×J）。
    // 取值理由见 config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS。
    double jb_stall_peak_cap_slots = config::JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS;
    // stall 峰值的衰减速度（ms/s）= `--jb-stall-decay`。"峰值记多久"：
    // **0 = 峰值永久保持**（把"近期最坏间隙"变成"历史最坏间隙"，极值实验）。
    // 取值理由见 config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC。
    double jb_stall_peak_decay_ms_per_sec = config::JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC;
    // stall 门阈值（包周期倍数）= `--jb-stall-threshold`。**≤ 0 = 关检测**
    // （每个到达间隔都进 J，回到裸 RFC 3550）——这是验证"stall 剔除到底有没有用"
    // 的唯一 A/B 手段。取值理由见 config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS。
    double jb_stall_threshold_packets = config::JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS;
    // 每次欠载事件抬升 target 下限的槽数 = `--jb-underrun-penalty`。
    // **0 = 关闭整条欠载反馈闭环**——分离"预测项"与"反馈项"各自贡献的关键对照。
    // 取值理由见 config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS。
    double jb_underrun_penalty_slots = config::JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS;
    // 每个 --jb-* 数值旋钮的来源（true = 命令行显式指定，false = 取默认值）。
    // 只用于启动时那一行 effective config 诊断，不参与任何运行时决策。
    struct JbOptionProvenance {
        bool capacity = false;
        bool jitter_gain = false;
        bool min_target = false;
        bool stall_peak_cap = false;
        bool stall_decay = false;
        bool stall_threshold = false;
        bool underrun_penalty = false;
    };
    JbOptionProvenance jb_option_provenance;
    // Phase 2 PCM concealment（产品默认开；JitterBuffer 组件本身默认关）：
    // 开 = 缺帧时重复上一个有效包 + 短淡出，超过连续上限转静音；
    // 关 = 缺帧直接静音（v1 行为）。连接属性，运行期不可切换。
    bool jb_pcm_concealment = true;
    // 播放路由起步（playback_switching_design.md §4）：true = PreferCurrent
    // （"自动切换播放设备"关；首流成功后钉住实际设备），false = FollowSystem
    // （跟随系统默认）。路由是连接属性，不持久化，每次连接按设置起步。
    bool playback_prefer_current = false;
    std::string server_ip = "127.0.0.1";
    std::uint16_t rpc_port = config::DEFAULT_RPC_PORT;
    // 仅覆盖 Server 通过 gRPC 下发的 UDP 端口；空值表示完全采用 Server 通告。
    std::optional<std::uint16_t> udp_force_port;
    std::string client_name = config::DEFAULT_CLIENT_NAME;
};

class ClientRuntime final {
public:
    ClientRuntime(asio::io_context& ioc, const ClientRuntimeConfig& config);
    ~ClientRuntime();

    ClientRuntime(const ClientRuntime&) = delete;
    ClientRuntime& operator=(const ClientRuntime&) = delete;

    bool start();
    void stop() noexcept;

    [[nodiscard]] RuntimeState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }
    // 当前音频错误（错误通道，不属于诊断快照）：None = 当前无未恢复错误。
    // 成功的恢复事务（restart_on_error / set_playback_device 非 Fatal）会
    // 清零——值语义是"正在发生"，不是"曾经发生"的锁存残值。
    [[nodiscard]] audio::AudioError last_audio_error() const noexcept
    {
        return last_audio_error_.load(std::memory_order_acquire);
    }
    // 音频错误事件纪元：last_audio_error 每次变化（置位新值 / 恢复清零）
    // 递增。轮询方以 epoch 变化检测错误事件，配合 last_audio_error() 读
    // 当前值——既能看到新错误，也能看到"已恢复"（epoch 变 + None）。
    [[nodiscard]] std::uint64_t audio_error_epoch() const noexcept
    {
        return audio_error_epoch_.load(std::memory_order_acquire);
    }
    const grpc::ConnectResult& connect_result() const noexcept { return connect_result_; }
    [[nodiscard]] double jb_water_level() const noexcept;
    [[nodiscard]] std::uint32_t jb_used_slots() const noexcept;
    [[nodiscard]] std::uint32_t jb_capacity_slots() const noexcept;
    [[nodiscard]] std::uint64_t jb_reanchor_count() const noexcept;
    [[nodiscard]] std::uint64_t jb_reanchor_sanity_rejections() const noexcept;
    [[nodiscard]] std::uint64_t jb_last_reanchor_sequence() const noexcept;
    [[nodiscard]] std::uint64_t heartbeat_ack_count() const noexcept { return udp_.heartbeat_ack_count(); }
    [[nodiscard]] std::uint32_t heartbeat_ack_misses() const noexcept { return udp_.consecutive_heartbeat_ack_misses(); }
    [[nodiscard]] std::int64_t heartbeat_ack_age_ms() const noexcept { return udp_.heartbeat_ack_age_ms(); }
    [[nodiscard]] bool udp_heartbeat_failed() const noexcept { return udp_.heartbeat_failed(); }
    [[nodiscard]] net::UdpTransportStats udp_stats() const noexcept
    {
        return udp_.stats();
    }
    [[nodiscard]] std::uint64_t udp_audio_frames_accepted() const noexcept { return udp_.audio_frames_accepted(); }
    [[nodiscard]] std::uint64_t udp_malformed_datagrams() const noexcept { return udp_.malformed_datagrams(); }
    [[nodiscard]] std::uint64_t udp_unexpected_sender_datagrams() const noexcept { return udp_.unexpected_sender_datagrams(); }
    // 当前学到的 UDP peer endpoint（HeartbeatAck 实际来源，建连时确定）；尚未学到返回 nullopt。
    [[nodiscard]] std::optional<asio::ip::udp::endpoint> learned_peer_endpoint() const noexcept { return udp_.learned_peer_endpoint(); }
    [[nodiscard]] std::uint64_t udp_wrong_session_acks() const noexcept { return udp_.wrong_session_acks(); }
    [[nodiscard]] std::uint64_t udp_audio_payload_mismatches() const noexcept { return udp_.audio_payload_mismatches(); }
    [[nodiscard]] std::uint64_t udp_non_audio_datagrams() const noexcept { return udp_.non_audio_datagrams(); }
    [[nodiscard]] std::uint64_t udp_heartbeat_handshake_send_attempts() const noexcept { return udp_.heartbeat_handshake_send_attempts(); }
    [[nodiscard]] std::uint64_t udp_heartbeat_ack_miss_events() const noexcept { return udp_.heartbeat_ack_miss_events(); }
    [[nodiscard]] std::uint64_t jb_push_accepted() const noexcept;
    [[nodiscard]] std::uint64_t jb_push_rejected() const noexcept;
    [[nodiscard]] std::uint64_t jb_push_rejected_late() const noexcept;
    [[nodiscard]] std::uint64_t jb_push_rejected_slot_busy() const noexcept;
    [[nodiscard]] std::uint64_t jb_push_rejected_invalid() const noexcept;
    [[nodiscard]] std::uint64_t jb_push_rejected_sanity() const noexcept;
    [[nodiscard]] std::uint64_t jb_pull_calls() const noexcept;
    [[nodiscard]] std::uint64_t jb_pull_frames() const noexcept;
    [[nodiscard]] std::uint64_t jb_pull_silence_frames() const noexcept;
    [[nodiscard]] std::uint64_t jb_fill_episodes() const noexcept;
    [[nodiscard]] std::uint64_t jb_fill_corrected_slots() const noexcept;
    [[nodiscard]] std::uint64_t jb_drop_episodes() const noexcept;
    [[nodiscard]] std::uint64_t jb_drop_skipped_slots() const noexcept;
    [[nodiscard]] std::uint64_t jb_reanchor_requests() const noexcept;
    [[nodiscard]] std::uint64_t jb_reanchor_cancels() const noexcept;
    [[nodiscard]] std::uint64_t playback_pull_calls() const noexcept;
    [[nodiscard]] std::uint64_t playback_pull_frames() const noexcept;
    [[nodiscard]] std::uint64_t playback_pull_silence_frames() const noexcept;

    [[nodiscard]] bool playback_running() const noexcept
    {
        return playback_ != nullptr && playback_->is_running();
    }

    // 本地播放生命的平行状态维度（playback_switching_design.md §3）。
    [[nodiscard]] audio::PlaybackState playback_state() const noexcept
    {
        return playback_ != nullptr ? playback_->state() : audio::PlaybackState::Inactive;
    }

    // 错误驱动的播放恢复：由 on_playback_event 即时 post 到 ioc 执行
    // （supervision tick 每 500ms 轮询兜底——两者都消费同一设备错误标志，
    // 双检查保证只执行一次）。观察标志并执行 PlaybackManager 的
    // restart_on_error 事务（路由模式推导目标 + fallback 链 + 重试上限；
    // playback_switching_design.md §5/§6）。事务在 ioc 线程就地执行
    // （stop+start，JB 不清空 = 结转），阻塞窗口内 heartbeat 定时器延迟一拍
    // （1s 间隔 / 5s 超时，无害）。链耗尽 → PlaybackState=Fatal，
    // supervision 随后按 Fatal 终止整个 runtime。
    // 线程安全（内部 lifecycle_mutex_）；非 Running 状态为 no-op。
    void service_playback_recovery() noexcept;

    // 系统默认设备变化跟随（FollowSystem 模式）：由 supervision tick 每
    // 500ms 调用（与 service_playback_recovery 同控制线程串行）。设备轮询与
    // 切换决策在 PlaybackManager::tick()（它持 AudioDeviceManager 引用）；
    // 本方法只做生命周期门禁 + lifecycle_mutex_ 串行化后转发。
    // Android 的 default_device 返回空 id（合成条目），PlaybackManager::tick
    // 在 Android 上为 no-op——Android 的设备跟随由推送模型驱动
    // （notify_devices_changed，playback_switching_design.md §5 rev2）。
    void service_default_device_follow() noexcept;

    // 监督轮询单实现（CLI control timer 与 C API supervision_main 共用，
    // 逐行同构的重复逻辑收敛于此）：依次执行错误驱动恢复 + 默认设备跟随，
    // 然后给出终态裁决。Continue = 无事；StopDegraded = 网络/控制面死亡；
    // StopFatal = 播放链耗尽。本函数只裁决不 stop（各前端停自己的 io_context）。
    enum class ControlPoll : std::uint8_t { Continue = 0,
        StopDegraded,
        StopFatal };
    ControlPoll poll_control() noexcept;

    // 设备集合变化推送入口（平台推送模型；Android AudioManager 回调经
    // C API 转发）。present_ids = 当前可选输出设备 id 全集（后端词汇，
    // 如 "android:N"）。任意线程可调用：内部 post 到 ioc 做 1s 合并去抖
    // （最新快照胜出），决策全部在 PlaybackManager::on_devices_changed
    // （跟随 / eager 回退 / PreferredDevice 自动切回）；调用方不做任何
    // 路由决策。未连接 / 非 Running 时为 no-op（快照仍记录为基线）。
    void notify_devices_changed(std::vector<audio::AudioDeviceId> present_ids) noexcept;

    // 显式切换播放设备（用户选择；nullopt = 跟随系统）。
    // 代理 PlaybackManager::set_playback_device（完整候选链 + 回滚），
    // 经 lifecycle_mutex_ 串行化；仅 Running 状态接受。
    // 返回切换结果；NotRunning = 未连接或状态非法。
    std::expected<audio::SwitchResult, audio::AudioError>
    set_playback_device(std::optional<audio::AudioDeviceId> target) noexcept;

    // 一次性聚合诊断快照（字段契约见 aqua/diagnostics/client_diagnostics_snapshot.h）。
    // CLI 日志与 C API / GUI 前端共用；各字段为原子近似读值，任意线程可调用。
    [[nodiscard]] aqua::diagnostics::ClientDiagnosticsSnapshot take_diagnostics_snapshot() const noexcept;

private:
    struct CallbackGate {
        explicit CallbackGate(ClientRuntime* owner) noexcept
            : owner(owner)
        {
        }

        CallbackGate(const CallbackGate&) = delete;
        CallbackGate& operator=(const CallbackGate&) = delete;

        template <typename Fn>
        void invoke(Fn&& fn) noexcept
        {
            std::lock_guard lock(mutex);
            if (owner != nullptr) {
                try {
                    fn(*owner);
                } catch (const std::exception& e) {
                    log_error_fmt("ClientRuntime asynchronous notification callback threw: {}", format_exception_message(e));
                } catch (...) {
                    log_error("ClientRuntime asynchronous notification callback threw unknown exception");
                }
            }
        }

        void detach() noexcept
        {
            std::lock_guard lock(mutex);
            owner = nullptr;
        }

        std::mutex mutex;
        ClientRuntime* owner = nullptr;
    };

    bool setup_playback(const audio::AudioFormat& format, std::uint32_t frame_count);
    void stop_locked() noexcept;
    std::uint32_t pull_playback(std::span<std::byte> output) noexcept;
    // 几何地板校正：用 pull_playback 观测到的实际 callback 帧数（而非 start
    // 时的请求值——backend 实际周期可能更大，尤其 frames_per_buffer=0 让
    // backend 自选时）重算 floor 并更新 controller。幂等（值未变不更新），
    // 在控制线程调用（start 末尾 + 每次 poll_control），覆盖设备切换事务后
    // callback 几何变化的情况。
    void sync_geometric_floor() noexcept;
    bool enter_starting() noexcept;
    bool enter_stopping() noexcept;
    void enter_stopped() noexcept;
    void on_playback_event(audio::AudioError error) noexcept;
    void on_network_liveness_failure(std::uint32_t consecutive_misses) noexcept;
    void on_reanchor_sanity_failure(std::uint64_t rejections) noexcept;
    // 控制面死亡（proto keepalive 首次非 Ok：传输中断或会话已不在）：
    // 直接 Degraded（supervision 观察到后 stop + 退出），不重试。
    void on_control_plane_dead(grpc::GrpcClient::KeepaliveStatus status) noexcept;
    // 设备事件合并窗口触发后的决策转发（ioc 线程；lifecycle_mutex_ 串行化）。
    void service_devices_changed() noexcept;
    // 错误通道维护：置位新错误 / 恢复清零，值变化时递增 audio_error_epoch_。
    void latch_audio_error(audio::AudioError error) noexcept;
    void clear_audio_error() noexcept;

    ClientRuntimeConfig config_;
    asio::io_context& ioc_;
    // 设备事件合并窗口定时器（notify_devices_changed；仅 ioc 线程访问）。
    asio::steady_timer device_event_timer_;
    std::unique_ptr<audio::AudioDeviceManager> device_mgr_;
    // 播放管理边界：ClientRuntime -> PlaybackManager -> AudioPlayback
    // （playback_switching_design.md；PlaybackState 由 manager 维护）。
    std::unique_ptr<audio::PlaybackManager> playback_;
    grpc::GrpcClient grpc_;
    net::UdpClient udp_;
    std::shared_ptr<audio::JitterBuffer> jb_;
    // Phase 0 网络观测（只观察不驱动 JB）：push strand 经 arrival observer 更新，
    // 诊断线程读 estimates()。shared_ptr 让 observer 回调持有， strand 残留
    // handler 析构后不野（UdpClient State 可能短暂存活）。
    std::shared_ptr<audio::JitterEstimator> estimator_;
    // Phase 1 自适应 target（jb_adaptive_target 开时创建）：同 strand 上
    // estimator → controller → jb.set_target_slots() 链路。
    std::shared_ptr<audio::TargetController> controller_;
    std::uint32_t frame_count_ = 0;
    std::uint32_t frame_bytes_ = 0;
    grpc::ConnectResult connect_result_;
    mutable std::mutex lifecycle_mutex_;
    std::atomic<RuntimeState> state_ { RuntimeState::Created };
    std::atomic<audio::AudioError> last_audio_error_ { audio::AudioError::None };
    // 错误事件纪元（latch/clear 时递增；见 audio_error_epoch()）。
    std::atomic<std::uint64_t> audio_error_epoch_ { 0 };
    // 设备事件合并窗口（仅 ioc 线程访问）：pending=true 期间新快照只覆盖
    // pending_device_ids_，窗口到期统一决策一次（蓝牙风暴合并）。
    bool device_event_pending_ = false;
    std::vector<audio::AudioDeviceId> pending_device_ids_;
    // 设备事件合并窗口时长（playback_switching_design.md §5 rev2：1s，
    // 最新快照胜出）。
    static constexpr auto kDeviceEventMergeWindow = std::chrono::seconds(1);
    // 设备类错误标志：backend event 线程置位，控制线程（supervision）
    // 在 service_playback_recovery() 中消费。回调线程不执行 restart
    // （stop/join/start 必须在控制线程，playback_switching_design.md §7）。
    std::atomic<bool> playback_device_error_pending_ { false };
    // 控制面死亡 latch（ping 线程置位；stop 时跳过必超时的 Disconnect）。
    std::atomic<bool> control_plane_dead_ { false };
    std::atomic<std::uint64_t> playback_pull_calls_ { 0 };
    std::atomic<std::uint64_t> playback_pull_frames_ { 0 };
    std::atomic<std::uint64_t> playback_pull_silence_frames_ { 0 };
    // ---- 几何地板校正（sync_geometric_floor）----
    // RT 线程（pull_playback）写 relaxed，控制线程读：最近一次 playback
    // callback 实际请求的帧数。这是"一次 callback 消耗的包数"的权威口径——
    // AudioStreamInfo::frames_per_burst 在 WASAPI legacy 下恒为 0、AAudio 下
    // 是设备原生 burst，都不能当 callback 帧数用。
    std::atomic<std::uint32_t> last_callback_frames_ { 0 };
    // 已应用到 controller 的几何地板（槽）。仅控制线程访问（setup_playback
    // 初始化为请求值口径，sync_geometric_floor 比对后再更新）。
    std::uint32_t applied_geometric_floor_slots_ = 0;
    std::shared_ptr<CallbackGate> callback_gate_;
};

} // namespace aqua::runtime

#endif // AQUA_RUNTIME_CLIENT_RUNTIME_H
