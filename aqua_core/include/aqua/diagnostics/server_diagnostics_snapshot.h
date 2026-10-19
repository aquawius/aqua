#ifndef AQUA_DIAGNOSTICS_SERVER_DIAGNOSTICS_SNAPSHOT_H
#define AQUA_DIAGNOSTICS_SERVER_DIAGNOSTICS_SNAPSHOT_H

// Server 诊断聚合快照：一次调用采集 ServerRuntime / capture / packetizer /
// AudioFrameQueue / dispatcher / UdpServer / SessionManager 的全部诊断量。
// CLI 调试日志与 C API（Android/GUI 前端）共用这一份字段契约。
//
// 值语义 POD；各字段为原子近似读值（relaxed），多线程并发下字段之间不保证
// 一致，仅供监控/显示使用，不用于控制决策。由
// ServerRuntime::take_diagnostics_snapshot() 采集，可在任意线程调用。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_format.h"
#include "aqua/audio/audio_switch_result.h"
#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/devices/route_policy.h"
#include "aqua/audio/capture/capture_manager.h"
#include "aqua/net/address/ip_family.h"
#include "aqua/net/udp/udp_transport.h"
#include "aqua/runtime/runtime_state.h"
#include "aqua/session/session_manager.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aqua::diagnostics {

struct ServerDiagnosticsSnapshot {
    // ---- 生命周期 ----
    runtime::RuntimeState state = runtime::RuntimeState::Created;
    audio::AudioError last_audio_error = audio::AudioError::None;
    bool capture_running = false;

    // ---- 音频契约（启动时解析的实际值）----
    audio::AudioFormat audio_format { };
    std::uint32_t frame_count = 0;

    // ---- capture（AudioCaptureStats，WASAPI 等后端统计）----
    struct CaptureStats {
        std::uint64_t audio_events = 0;
        std::uint64_t packet_queries = 0;
        std::uint64_t packet_empty = 0;
        std::uint64_t packets_ready = 0;
        std::uint64_t get_buffer_success = 0;
        std::uint64_t callbacks = 0;
        std::uint64_t silent_callbacks = 0;
        std::uint64_t synthetic_silence_blocks = 0;
        std::uint64_t generated_silence_frames = 0;
        std::uint64_t starved_events = 0;
        std::uint64_t starved_ms = 0;
        audio::AudioCaptureState state = audio::AudioCaptureState::Active;

        // ---- 吞吐量（callbacks 只知道"几次"，不知道"多少帧"；仅真实 packet）----
        std::uint64_t captured_frames = 0;
        std::uint64_t captured_bytes = 0;
        // ---- GetNextPacketSize 可变性观测（packet_frames_min=0 = 尚无样本）----
        std::uint32_t packet_frames_last = 0;
        std::uint32_t packet_frames_min = 0;
        std::uint32_t packet_frames_max = 0;
        // ---- starvation 形状（累计值分不出 100×50ms 与 1×5000ms）----
        std::uint64_t current_starved_ms = 0; // 当前这一轮 starved 已持续 ms（非 starved = 0）
        std::uint64_t max_starved_ms = 0; // 本次运行最长一轮 starved 时长
        // ---- 端点几何（2026-10-19 补）----
        // 端点缓冲帧数：采集侧延迟的**硬地板**（WASAPI 实测 1056 帧 @48k = 22ms）。
        // AudioCaptureInfo::frames_per_buffer 一直是公开契约，却没进任何快照——
        // 于是"我的采集延迟下限是多少"这个问题在 server 侧无法回答。
        std::uint32_t endpoint_buffer_frames = 0; // 0 = 未上报
        // 事件归属契约的"唯一可观测量"（capture_manager 的同名计数器）：
        // 非零说明 provenance 过滤真的拦下了东西。
        std::uint64_t stale_events_dropped = 0;
    } capture;

    // ---- capture_switch（CaptureManager 管理级状态，capture_switching_design.md §7）----
    // 与流级 capture.state 正交：这里反映设备切换事务与路由，而非时间轴。
    struct CaptureSwitchStats {
        audio::CaptureSwitchState state = audio::CaptureSwitchState::Inactive;
        // 意图归属（RoutePolicy 的诊断投影；取值即 aqua_route_mode 编码）。
        audio::RouteIntentOwner route = audio::RouteIntentOwner::None;
        audio::AudioCaptureSource source = audio::AudioCaptureSource::OUTPUT_LOOPBACK;
        std::string active_device_id; // 实际 resolve 并成功打开的设备（空 = 未知/未运行）
        std::string requested_device_id; // sticky 用户意图（preferred；空 = 跟随系统）
        audio::SwitchOutcome last_outcome = audio::SwitchOutcome::None;
        audio::AudioError last_switch_error = audio::AudioError::None;
        std::uint32_t last_switch_duration_ms = 0; // 最近一次切换事务耗时（stop->候选链->start）
    } capture_switch;

    // ---- packetizer（capture RT 路径上的分帧）----
    struct PacketizerStats {
        std::uint64_t input_blocks = 0;
        std::uint64_t input_bytes = 0;
        std::uint64_t frames_emitted = 0;
        std::uint64_t rejected_unaligned_blocks = 0;
        // 因采集端点切换而丢弃的 pending 半帧次数（保留 sequence，不重置时间线）。
        std::uint64_t pending_discards = 0;
    } packetizer;

    // ---- AudioFrameQueue（SPSC：capture RT -> dispatcher worker）----
    struct QueueStats {
        std::uint64_t accepted_frames = 0;
        std::uint64_t consumed_frames = 0;
        std::uint64_t dropped_frames = 0;
        std::uint32_t depth_slots = 0;
        std::uint32_t high_watermark_slots = 0; // 运行期峰值深度（与瞬时 depth 互补）
        // 积压的**时间**维度（2026-10-19 补）：depth_slots 是槽数，槽数不告诉你
        // 延迟（要乘包周期）。这里是 depth × 包周期，在快照边界派生。
        double backlog_ms = 0.0; // 当前积压对应的毫秒数（0 = 无法换算）
    } queue;

    // ---- dispatcher（worker：编码 + 广播）----
    struct DispatcherStats {
        std::uint64_t frames_encoded = 0;
        std::uint64_t frames_broadcast = 0;
        std::uint64_t frames_without_clients = 0;
        std::uint64_t encode_failures = 0;
        std::uint64_t dispatch_failures = 0;
        std::uint64_t dropped_frames = 0; // **口径**：handoff 队列满丢（与
            // queue.dropped_frames 同源、同一数字的别名；**不是**"网络队列满"，
            // dispatcher 自己没有独立的网络队列丢包计数——旧注释写的是后者，
            // 与实现不符，2026-10-19 更正）。
        std::uint64_t published_frames = 0;
        std::uint64_t worker_wakeups = 0;

        // ---- pacing / 追赶（2026-10-19 补）----
        // 这组回答"server 发得顺不顺"：catchup_drains 稳态必须为 0；>0 表示
        // worker 被调度饿死过、或生产瞬时超过了发送能力（client 侧会看到周期性的
        // 缺口）。这组数据此前只出现在 stop 日志行里，诊断快照里一个都没有。
        std::uint64_t paced_sends = 0; // pacing 模式下发出的包数（含追赶拍）
        std::uint64_t catchup_drains = 0; // 追赶拍次数（不是"追赶事件次数"）
        bool catchup_active = false; // 此刻是否正在追赶（逐拍状态量）
        std::uint64_t pacing_interval_ns = 0; // 有效包周期（ns）；0 = pacing 关闭
        // 追赶阈值（编译期常量，恒已知但此前不可见）：没有它们，queue.depth_slots
        // 的数值无法解读（6 到底是高于还是低于 8 的追赶深度？）。
        std::uint32_t catchup_depth_slots = 0; // 进入追赶的积压阈值
        std::uint32_t catchup_low_water_slots = 0; // 退出追赶的低水位（迟滞）
        std::uint32_t catchup_speedup = 0; // 追赶时的加速倍数
        std::uint32_t max_catchup_sends = 0; // 单拍最多补发的包数
    } dispatcher;

    // ---- net：UDP 数据面 + session ----
    struct Net {
        // 逐族 transport 计数与队列（INV-6：绑定/通告/诊断/统计都逐族）。
        // served_* 用于区分"**没配**这一族"与"配了但零流量"——后者才是故障信号，
        // 只报合计会把这两件事混成同一行数字。
        struct PerFamilyTransport {
            net::UdpTransportStats v4 { };
            net::UdpTransportStats v6 { };
            bool served_v4 = false;
            bool served_v6 = false;

            [[nodiscard]] net::FamilySet served() const noexcept
            {
                return net::FamilySet { served_v4, served_v6 };
            }

            // 两族合计（既有诊断口径：C API 的 net.rx/tx 与渲染的紧凑行都用它）。
            // 由两个分项相加得到，不额外维护第二份总量 —— 不存在漂移的可能。
            // tx_queue_depth 是瞬时量，取较大者（相加无意义）。
            [[nodiscard]] net::UdpTransportStats total() const noexcept
            {
                auto t = v4;
                t.rx_packets += v6.rx_packets;
                t.rx_bytes += v6.rx_bytes;
                t.rx_errors += v6.rx_errors;
                t.rx_unreachable += v6.rx_unreachable;
                t.tx_packets += v6.tx_packets;
                t.tx_bytes += v6.tx_bytes;
                t.tx_errors += v6.tx_errors;
                t.tx_dropped += v6.tx_dropped;
                t.tx_enqueue_failures += v6.tx_enqueue_failures;
                t.tx_queue_depth = std::max(t.tx_queue_depth, v6.tx_queue_depth);
                // 2026-10-19 补：连续错误数取较大者（"正在退避"是状态量，相加无意义），
                // 错误码取非零的那个（一族干净时它的 last_error_code 为 0）。
                t.rx_consecutive_errors = std::max(t.rx_consecutive_errors, v6.rx_consecutive_errors);
                if (t.rx_last_error_code == 0) {
                    t.rx_last_error_code = v6.rx_last_error_code;
                }
                return t;
            }
        } transport;
        // heartbeat（client→server 唯一包型：建立 + 续命）/ 协议分类。
        // 合计沿用既有诊断口径；逐族分项是 INV-6 的兑现：收包健康检查必须按族判定
        // "这一族的 heartbeat 停了没有"——合计口径下 v4 在跳时会掩盖 v6 收包路径死亡
        // （见 ServerRuntime::check_udp_receive_health）。两族恒满足 合计 == v4 + v6。
        std::uint64_t heartbeat_received = 0;
        std::uint64_t heartbeat_rejected = 0;
        std::uint64_t heartbeat_received_v4 = 0;
        std::uint64_t heartbeat_received_v6 = 0;
        std::uint64_t heartbeat_rejected_v4 = 0;
        std::uint64_t heartbeat_rejected_v6 = 0;
        std::uint64_t sessions_established = 0;
        std::uint64_t sessions_refreshed = 0;
        std::uint64_t heartbeat_ack_attempts = 0;
        std::uint64_t malformed_datagrams = 0;
        std::uint64_t non_heartbeat_datagrams = 0;
    } net;

    // ---- session（SessionManager::Stats + 当前存活数）----
    struct SessionStats {
        std::size_t active = 0; // 当前仍存在的 session 数
        std::uint64_t created = 0;
        std::uint64_t connected = 0;
        std::uint64_t refreshed = 0;
        std::uint64_t removed = 0;
        std::uint64_t expired = 0;
        std::uint64_t removed_by_clear = 0;
        // 2026-10-19 补：活跃度年龄。active 只说"有几个"，不说"它们多久没动"。
        // "active=1 但 oldest_age=29s"意味着那个 session 下一轮就会被 reap
        // （session_timeout 默认 5s）——半死的连接在聚合计数里完全看不出来。
        // 多 session 时 oldest 比单一 age 更有意义（最老的那个才是要死的）。
        // SessionManager::activity_age() 早就写好了，此前**零调用点**。
        std::size_t activity_count = 0; // 有活跃度记录的 session 数
        std::uint64_t activity_oldest_age_ms = 0; // 最老的那个多久没动（ms）
        std::uint64_t activity_newest_age_ms = 0; // 最新的那个多久没动（ms）
    } session;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_SERVER_DIAGNOSTICS_SNAPSHOT_H
