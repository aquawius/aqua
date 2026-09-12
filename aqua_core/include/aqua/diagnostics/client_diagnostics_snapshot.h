#ifndef AQUA_DIAGNOSTICS_CLIENT_DIAGNOSTICS_SNAPSHOT_H
#define AQUA_DIAGNOSTICS_CLIENT_DIAGNOSTICS_SNAPSHOT_H

// Client 诊断聚合快照：一次调用采集 ClientRuntime / UdpClient / JitterBuffer /
// playback 消费侧的全部诊断量。CLI 调试日志与 C API（Android/GUI 前端）共用
// 这一份字段契约，避免两个前端各自维护一套字段集合。
//
// 值语义 POD；各字段为原子近似读值（relaxed），多线程并发下字段之间不保证
// 一致，仅供监控/显示使用，不用于控制决策。由
// ClientRuntime::take_diagnostics_snapshot() 采集，可在任意线程调用。

#include "aqua/audio/audio_error.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_manager.h"
#include "aqua/audio/playback/playback_state.h"
#include "aqua/net/udp/udp_transport.h"
#include "aqua/runtime/runtime_state.h"

#include <cstdint>

namespace aqua::diagnostics {

struct ClientDiagnosticsSnapshot {
    // ---- 生命周期 ----
    // 注：音频错误不是诊断快照字段（快照 = 组件状态，不承担错误传递）。
    // 错误经 ClientRuntime::last_audio_error()/audio_error_epoch() 独立
    // 通道上报（epoch 变化检测 + 恢复清零语义）。
    runtime::RuntimeState state = runtime::RuntimeState::Created;
    bool playback_running = false;
    // 本地播放生命的平行状态维度（playback_switching_design.md §3）。
    audio::PlaybackState playback_state = audio::PlaybackState::Inactive;

    // ---- 播放路由与切换事务（playback_switching_design.md §9）----
    audio::PlaybackRouteMode route_mode = audio::PlaybackRouteMode::FollowSystem;
    // 最近一次切换事务的结果（None = 尚未发生切换）。
    audio::SwitchResult switch_result { };
    // 请求设备（PreferredDevice 时有值；空 = 无显式请求）。
    audio::AudioDeviceId requested_device_id;

    // ---- net：UDP 数据面 + heartbeat 建连/续命 ----
    struct Net {
        // heartbeat 建连握手 / 稳态路径探活（字段名为历史契约，含义见 udp_client.h）
        std::uint64_t heartbeat_ack_count = 0; // 收到的 ACK 总数（建连确认 + 稳态回执）
        std::uint32_t heartbeat_ack_misses = 0; // 当前连续未收到 ACK 的 heartbeat 数
        std::int64_t heartbeat_ack_age_ms = 0; // 距最近一次 ACK 的毫秒数
        bool heartbeat_failed = false; // liveness 失败锁存（握手期/稳态任一超限即置位，只置一次）
        std::uint64_t heartbeat_handshake_send_attempts = 0; // 握手期 heartbeat 发送总数（建连后冻结）
        std::uint64_t heartbeat_ack_miss_events = 0; // miss tick 累计数（阈值穿越不单独计数）
        // ---- Phase 0 网络观测（JitterEstimator 纯观察：只进诊断，不驱动 JB）----
        double estimator_jitter_ms = 0.0; // RFC 3550 interarrival jitter（观测量 ≠ target）
        double estimator_base_delay_ms = 0.0; // 路径底噪（transit 累积最小）
        double estimator_transit_ms = 0.0; // 当前相对 transit
        std::uint64_t estimator_reordered_packets = 0; // 乱序到达（窗内落后）
        std::uint64_t estimator_duplicate_packets = 0; // 重复到达
        std::uint64_t estimator_late_packets = 0; // 落后观测窗之外
        // transport 计数与队列
        net::UdpTransportStats transport { };
        // datagram 分类计数
        std::uint64_t audio_frames_accepted = 0;
        // 音频接收序列缺口统计（见 UdpClient）：“收到流出现缺口”，非直接叫丢包。
        std::uint64_t rx_audio_sequence_gap_events = 0;
        std::uint64_t rx_audio_sequence_missing_frames = 0;
        std::uint64_t malformed_datagrams = 0;
        std::uint64_t unexpected_sender_datagrams = 0;
        std::uint64_t wrong_session_acks = 0;
        std::uint64_t audio_payload_mismatches = 0;
        std::uint64_t non_audio_datagrams = 0;
    } net;

    // ---- JitterBuffer ----
    struct JitterBufferStats {
        double water_level = 0.0; // W = lead_slots / N
        std::uint32_t used_slots = 0;
        std::uint32_t capacity_slots = 0;
        std::uint64_t reanchor_count = 0;
        std::uint64_t reanchor_requests = 0;
        std::uint64_t reanchor_cancels = 0;
        std::uint64_t reanchor_sanity_rejections = 0;
        std::uint64_t last_reanchor_sequence = 0;
        std::uint64_t push_accepted = 0;
        std::uint64_t push_rejected = 0;
        std::uint64_t push_rejected_late = 0;
        std::uint64_t push_rejected_slot_busy = 0;
        std::uint64_t push_rejected_invalid = 0;
        std::uint64_t push_rejected_sanity = 0;
        std::uint64_t pull_calls = 0;
        std::uint64_t pull_frames = 0;
        std::uint64_t pull_silence_frames = 0;
        std::uint64_t fill_episodes = 0;
        std::uint64_t fill_corrected_slots = 0;
        std::uint64_t drop_episodes = 0;
        std::uint64_t drop_skipped_slots = 0;

        // ---- Phase 2 欠载预算 + PCM concealment（细则 §8/§9/§11/§14）----
        // underrun = 播放头推进到"没有真实 PCM 可用"的 slot：conceal 开启后
        // 这些帧被 repeat-last 掩盖（计 underrun、不计 silence）；pre-roll 与
        // 低水位 Hold 的静音是时间轴修正，不计入。
        std::uint64_t underrun_events = 0; // 缺帧 run 的次数（相邻缺帧 slot 合并为一次）
        std::uint64_t underrun_frames = 0; // 掩盖帧 + 缺帧静音帧
        std::uint64_t max_consecutive_underrun_slots = 0; // 单次最长缺帧 slot 数（验收"单次≤N 包"）
        std::uint64_t concealed_slots = 0; // 被 repeat-last 掩盖的 slot 数
        std::uint64_t concealed_saturated_slots = 0; // 超连续上限退回静音的 slot 数
        std::uint64_t late_useful_packets = 0; // 迟到但落在 conceal 窗口内（本可用）的包数
        // 派生比值（同快照一次算出，前端不用自己拿 frame_count 换算）：
        double underrun_ratio = 0.0; // underrun_frames / pull_frames
        double fill_duty = 0.0; // Fill 慢放多播的帧占比（fill_corrected_slots×F / pull_frames）
        double drop_duty = 0.0; // Drop 跳过 slot 的帧占比（drop_skipped_slots×F / pull_frames）

        // ---- Gauge / 当前态（与累计 counter 互补；JB 内部原子镜像，可跨线程读）----
        std::uint32_t lead_slots = 0; // lead = highest - play + 1（绝对值；water_level 是归一化的）
        double lead_ms = 0.0; // lead 换算毫秒（与 target_ms 同口径；细则 §11 要求 lead_slots+lead_ms 同快照）
        std::uint32_t target_slots = 0; // 当前 target（固定模式 = 构造值；自适应 = controller 输出）
        double target_ms = 0.0; // target 换算毫秒（包时长 = F×1000/sample_rate）
        std::uint64_t play_sequence = 0; // 播放头序列（未锚定 = 0）
        std::uint64_t highest_received_sequence = 0; // 已收到的最高序列
        // 断流形状：consecutive = 当前连续静音 run（出现真实数据归零）；max = 本次运行最长 run。
        std::uint64_t consecutive_silence_frames = 0;
        std::uint64_t max_silence_run_frames = 0;
        // 当前时间轴修正方向：0=None 1=Filling 2=Dropping（JitterBufferEpisodeState）。
        std::int32_t episode_state = 0;
        bool reanchor_pending = false; // 有待 consumer 应用（尚未处理）的 reanchor 请求
        std::uint64_t reanchor_target_sequence = 0; // 待应用目标序列（无 = 0）
    } jitter_buffer;

    // ---- playback 消费侧（ClientRuntime 自有统计，跨 JB/playback 观测）----
    struct PlaybackStats {
        std::uint64_t pull_calls = 0;
        std::uint64_t pull_frames = 0;
        std::uint64_t pull_silence_frames = 0;
    } playback;

    // ---- playback 输出流实际运行参数（后端 open 后回读；backend=None = 未运行）----
    audio::AudioStreamInfo stream;

    // ---- Buffer 决策层观测（末尾追加）----
    // 上面的 jitter_buffer 组给的是"结果与累计计数"（水位、episode、underrun、
    // concealment 计数）；本组给的是"决策与阈值"——回答**为什么** target /
    // 水位 / 掩盖是现在这样。
    //
    // 为什么单独成组而不是插进上面的分组：C API 结构体（aqua_client_diagnostics_t）
    // 只能末尾追加才能保 ABI，中间插入会把其后所有字段的偏移整体挪位。本组是
    // ClientDiagnosticsSnapshot 的最后一个成员，组内继续追加同样安全。
    //
    // 三部分来源不同，故也不塞进上面的单个 `if (jb_)`/`if (estimator_)` 块：
    //   controller（决策层） / estimator（观测层尾部） / jb（执行层带与 run）。
    struct JitterControl {
        // ---- 决策层：TargetController（仅自适应模式；adaptive=false 时全 0）----
        bool adaptive = false; // target 是否由 TargetController 输出（false = --jb-fixed-target 固定模式）
        std::uint32_t desired_slots = 0; // 未限速期望值；与 target_slots 不等 = 被限速/dwell/死区按住
        std::uint32_t min_slots = 0; // 生效下限 = max(--jb-min-target, 几何地板 + 1)
        std::uint32_t max_slots = 0; // 结构上限 = 2/3 × capacity
        std::int32_t margin_source = 0; // margin 胜出方：0=kJ 1=stall_peak
        std::int32_t path = 0; // 本拍收敛路径：0=steady 1=rise 2=fall 3=dwell_lock 4=deadband 5=no_time_base
        bool floor_bound = false; // desired 被下限抬起（margin 失算，安全网托住）
        bool cap_bound = false; // desired 被结构上限夹住（正在兜底）
        double underrun_penalty = 0.0; // 欠载反馈抬升量（槽）；>0 = 闭环在工作（预期，非故障）
        double dwell_remaining_ms = 0.0; // 涨后锁跌剩余（ms）；>0 = 正在锁跌（峰值保持）
        double fall_room_slots = 0.0; // 本拍跌侧限速额度（槽）；解释"这一拍为什么只降一格"

        // ---- 观测层尾部：JitterEstimator 的 stall 侧与到达节奏 ----
        std::uint64_t stall_events = 0; // 被 stall 门剔除的断流次数（不进 J）
        double stall_peak_ms = 0.0; // 近期最坏到达间隙的衰减最大值（margin 峰值项的输入）
        double last_stall_gap_ms = 0.0; // 最近一次 stall 的到达间隔
        double arrival_interval_ms = 0.0; // 最近一个按序包的到达间隔

        // ---- 执行层：水位带与当前缺帧/掩盖 run ----
        // 四个带值同源（JitterBuffer::bands() 单快照）：解释"此刻为什么 Fill/Drop"。
        // target 落在带外才动作，故看动作前先看带。
        std::uint32_t band_warning_low = 0;
        std::uint32_t band_normal_low = 0;
        std::uint32_t band_normal_high = 0;
        std::uint32_t band_warning_high = 0;
        std::uint32_t conceal_run_slots = 0; // 当前连续掩盖槽数（0 = 未在掩盖）
        std::uint32_t underrun_run_slots = 0; // 当前连续缺帧槽数（0 = 正常）
    } jitter_control;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_CLIENT_DIAGNOSTICS_SNAPSHOT_H
