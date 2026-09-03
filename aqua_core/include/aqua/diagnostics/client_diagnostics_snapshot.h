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

    // ---- net：UDP 数据面 + HELLO 保活 ----
    struct Net {
        // HELLO / liveness
        std::uint64_t hello_ack_count = 0; // 收到的 HELLO_ACK 总数
        std::uint32_t hello_ack_misses = 0; // 当前连续未收到 ACK 的 HELLO 数
        std::int64_t hello_ack_age_ms = 0; // 距最近一次 ACK 的毫秒数
        bool hello_failed = false; // HELLO 保活已判定失败（终态）
        std::uint64_t hello_send_attempts = 0; // HELLO 发送尝试总数
        std::uint64_t hello_ack_miss_events = 0; // “连续 miss 达到阈值”事件总数
        // transport 计数与队列
        net::UdpTransportStats transport { };
        // datagram 分类计数
        std::uint64_t audio_frames_accepted = 0;
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
    } jitter_buffer;

    // ---- playback 消费侧（ClientRuntime 自有统计，跨 JB/playback 观测）----
    struct PlaybackStats {
        std::uint64_t pull_calls = 0;
        std::uint64_t pull_frames = 0;
        std::uint64_t pull_silence_frames = 0;
    } playback;

    // ---- playback 输出流实际运行参数（后端 open 后回读；backend=None = 未运行）----
    audio::AudioStreamInfo stream;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_CLIENT_DIAGNOSTICS_SNAPSHOT_H
