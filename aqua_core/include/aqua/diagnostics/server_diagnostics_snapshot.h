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
#include "aqua/audio/capture/capture_manager.h"
#include "aqua/net/udp/udp_transport.h"
#include "aqua/runtime/runtime_state.h"
#include "aqua/session/session_manager.h"

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
    } capture;

    // ---- capture_switch（CaptureManager 管理级状态，capture_switching_design.md §7）----
    // 与流级 capture.state 正交：这里反映设备切换事务与路由，而非时间轴。
    struct CaptureSwitchStats {
        audio::CaptureSwitchState state = audio::CaptureSwitchState::Inactive;
        audio::CaptureRouteMode route = audio::CaptureRouteMode::FollowSystem;
        audio::AudioCaptureSource source = audio::AudioCaptureSource::OUTPUT_LOOPBACK;
        std::string active_device_id; // 实际 resolve 并成功打开的设备（空 = 未知/未运行）
        std::string requested_device_id; // sticky 用户意图（preferred；空 = 跟随系统）
        audio::SwitchOutcome last_outcome = audio::SwitchOutcome::None;
        audio::AudioError last_switch_error = audio::AudioError::None;
    } capture_switch;

    // ---- packetizer（capture RT 路径上的分帧）----
    struct PacketizerStats {
        std::uint64_t input_blocks = 0;
        std::uint64_t input_bytes = 0;
        std::uint64_t frames_emitted = 0;
        std::uint64_t rejected_unaligned_blocks = 0;
    } packetizer;

    // ---- AudioFrameQueue（SPSC：capture RT -> dispatcher worker）----
    struct QueueStats {
        std::uint64_t accepted_frames = 0;
        std::uint64_t consumed_frames = 0;
        std::uint64_t dropped_frames = 0;
        std::uint32_t depth_slots = 0;
    } queue;

    // ---- dispatcher（worker：编码 + 广播）----
    struct DispatcherStats {
        std::uint64_t frames_encoded = 0;
        std::uint64_t frames_broadcast = 0;
        std::uint64_t frames_without_clients = 0;
        std::uint64_t encode_failures = 0;
        std::uint64_t dispatch_failures = 0;
        std::uint64_t dropped_frames = 0; // 网络队列满，入队前丢弃
        std::uint64_t published_frames = 0;
        std::uint64_t worker_wakeups = 0;
    } dispatcher;

    // ---- net：UDP 数据面 + session ----
    struct Net {
        // transport 计数与队列
        net::UdpTransportStats transport { };
        // HELLO / 协议分类
        std::uint64_t hello_received = 0;
        std::uint64_t hello_rejected = 0;
        std::uint64_t sessions_established = 0;
        std::uint64_t sessions_refreshed = 0;
        std::uint64_t hello_ack_attempts = 0;
        std::uint64_t malformed_datagrams = 0;
        std::uint64_t non_hello_datagrams = 0;
    } net;

    // ---- session（SessionManager::Stats + 当前存活数）----
    struct SessionStats {
        std::size_t active = 0; // 当前仍存在的 session 数
        std::uint64_t created = 0;
        std::uint64_t connected = 0;
        std::uint64_t refreshed = 0;
        std::uint64_t removed = 0;
        std::uint64_t expired = 0;
        std::uint64_t clear_removed = 0;
    } session;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_SERVER_DIAGNOSTICS_SNAPSHOT_H
