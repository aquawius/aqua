#include "aqua/diagnostics/snapshot_view.h"

#include "aqua/audio/audio_error.h"
#include "aqua/audio/audio_switch_result.h"
#include "aqua/audio/buffer/target_controller.h"
#include "aqua/audio/capture/audio_capture.h"
#include "aqua/audio/capture/capture_state.h"
#include "aqua/audio/devices/route_policy.h"
#include "aqua/audio/playback/audio_playback.h"
#include "aqua/audio/playback/playback_state.h"
#include "aqua/runtime/runtime_state.h"

#include <format>
#include <string>

namespace aqua::diagnostics {

namespace {

    // 紧凑音频格式：2ch/48k/enc3（Hz 整除 1000 时简写为 k）。
    std::string fmt_audio(const audio::AudioFormat& f)
    {
        std::string sr = (f.sample_rate >= 1000 && f.sample_rate % 1000 == 0)
            ? std::format("{}k", f.sample_rate / 1000)
            : std::to_string(f.sample_rate);
        return std::format("{}ch/{}/enc{}", f.channels, sr, static_cast<int>(f.encoding));
    }

    const char* jb_episode(int state)
    {
        return state == 1 ? "filling" : (state == 2 ? "dropping" : "none");
    }

} // namespace

// ============================================================
// ServerSnapshotView
// ============================================================

ServerSnapshotView::ServerSnapshotView(audio::AudioCaptureSource capture_source)
    : capture_source_(capture_source)
{
}

std::string ServerSnapshotView::render_state(const ServerDiagnosticsSnapshot& s, std::uint16_t udp_port) const
{
    FieldBlock b;
    b.field("state", std::string_view(runtime::runtime_state_name(s.state)))
        .field("sess", s.session.active)
        .field("udp", udp_port);
    return b.str();
}

std::string ServerSnapshotView::render_audio(const ServerDiagnosticsSnapshot& s) const
{
    const auto& cs = s.capture_switch;
    FieldBlock b;
    b.field("capture", s.capture_running)
        .field("err", std::string_view(audio::audio_error_name(s.last_audio_error)))
        .field("fmt", fmt_audio(s.audio_format))
        .field("F", s.frame_count)
        .field("src", static_cast<int>(capture_source_))
        .field("cstate", std::string_view(audio::capture_state_name(s.capture.state)))
        .field("sw", std::string_view(audio::capture_switch_state_name(cs.state)))
        .field("route", std::string_view(audio::route_intent_owner_label(cs.route)));
    // 只有 sticky 用户意图才值得显示设备：跟随系统时 requested_device_id 为空，
    // 显示出来只会让人以为路由被钉住了。
    if (cs.route == audio::RouteIntentOwner::User && !cs.requested_device_id.empty()) {
        b.field("dev", cs.requested_device_id);
    }
    b.field("ls", std::format("{}/{}ms", audio::switch_outcome_name(cs.last_outcome), cs.last_switch_duration_ms))
        .field("cf", s.capture.captured_frames)
        .field("cb", s.capture.captured_bytes)
        .field("pkt", std::format("{}/{}/{}", s.capture.packet_frames_last, s.capture.packet_frames_min, s.capture.packet_frames_max))
        .field("stv", std::format("{}/{}", s.capture.current_starved_ms, s.capture.max_starved_ms));
    return b.str();
}

std::string ServerSnapshotView::render_capture(const ServerDiagnosticsSnapshot& s) const
{
    const auto& c = s.capture;
    FieldBlock b;
    b.rate("ev", cap_audio_events_, c.audio_events)
        .rate("pq", cap_packet_queries_, c.packet_queries)
        .rate("pe", cap_packet_empty_, c.packet_empty)
        .rate("pr", cap_packets_ready_, c.packets_ready)
        .rate("gb", cap_get_buffer_, c.get_buffer_success)
        .rate("cbk", cap_callbacks_, c.callbacks)
        .rate("scb", cap_silent_callbacks_, c.silent_callbacks)
        .rate("ssb", cap_synth_silence_, c.synthetic_silence_blocks)
        .rate("gsf", cap_gen_silence_frames_, c.generated_silence_frames)
        .rate("se", cap_starved_events_, c.starved_events)
        .rate("stv_ms", cap_starved_ms_, c.starved_ms)
        .rate("cf", cap_captured_frames_, c.captured_frames)
        .rate("cby", cap_captured_bytes_, c.captured_bytes)
        // 端点几何：缓冲帧数 = 采集侧延迟的硬地板；stale = 事件归属契约的可观测量。
        .field("epbuf", c.endpoint_buffer_frames)
        .rate("stale", cap_stale_, c.stale_events_dropped)
        // 引擎断流（切歌的典型场景、xrun 的潜在来源）：此前只有 debug 日志。
        .rate("disc", cap_disc_, c.data_discontinuity_events);
    return b.str();
}

std::string ServerSnapshotView::render_packetizer(const ServerDiagnosticsSnapshot& s) const
{
    const auto& p = s.packetizer;
    FieldBlock b;
    b.rate("blk", pktz_blocks_, p.input_blocks)
        .rate("by", pktz_bytes_, p.input_bytes)
        .rate("frm", pktz_frames_, p.frames_emitted)
        .rate("unal", pktz_unaligned_, p.rejected_unaligned_blocks)
        .rate("disc", pktz_discards_, p.pending_discards);
    return b.str();
}

std::string ServerSnapshotView::render_queue(const ServerDiagnosticsSnapshot& s) const
{
    const auto& q = s.queue;
    FieldBlock b;
    b.field("depth", q.depth_slots)
        .field("hwm", q.high_watermark_slots)
        .field("hwm_r", q.high_watermark_slots_recent)
        .field("backlog_ms", q.backlog_ms, 2)
        .rate("acc", queue_accepted_, q.accepted_frames)
        .rate("con", queue_consumed_, q.consumed_frames)
        .rate("drop", queue_dropped_, q.dropped_frames);
    return b.str();
}

std::string ServerSnapshotView::render_dispatcher(const ServerDiagnosticsSnapshot& s) const
{
    const auto& d = s.dispatcher;
    FieldBlock b;
    b.rate("enc", dsp_encoded_, d.frames_encoded)
        .rate("bc", dsp_broadcast_, d.frames_broadcast)
        .rate("nocl", dsp_no_clients_, d.frames_without_clients)
        .rate("ef", dsp_encode_fail_, d.encode_failures)
        .rate("df", dsp_dispatch_fail_, d.dispatch_failures)
        .rate("drop", dsp_dropped_, d.dropped_frames)
        .rate("pub", dsp_published_, d.published_frames)
        .rate("wake", dsp_wakeups_, d.worker_wakeups)
        // pacing / 追赶：catchup_drains 稳态必须为 0（>0 = worker 被饿死过或生产
        // 超过了发送能力）。阈值一起给出，depth 才能解读。
        .rate("paced", dsp_paced_, d.paced_sends)
        .rate("catchup", dsp_catchup_, d.catchup_drains)
        .field("catchup_on", d.catchup_active)
        .field("period_ns", d.pacing_interval_ns)
        .field("cu_depth", d.catchup_depth_slots)
        .field("cu_low", d.catchup_low_water_slots)
        .field("cu_speed", d.catchup_speedup)
        .field("cu_max", d.max_catchup_sends);
    return b.str();
}

std::string ServerSnapshotView::render_net(const ServerDiagnosticsSnapshot& s) const
{
    // 合计口径与两族合计一致（见 snapshot.net.transport.total()）；served 直接回答
    // "这个实例在服务哪几族"——默认启动下应为 v4+v6。
    const auto& pf = s.net.transport;
    const auto t = pf.total();
    FieldBlock b;
    b.field("served", pf.served().to_string())
        .rate("rx", net_rx_, t.rx_packets)
        .rate("rxB", net_rx_bytes_, t.rx_bytes)
        .rate("rxerr", net_rx_err_, t.rx_errors)
        .rate("rxunr", net_rx_unr_, t.rx_unreachable)
        .rate("tx", net_tx_, t.tx_packets)
        .rate("txB", net_tx_bytes_, t.tx_bytes)
        .rate("txerr", net_tx_err_, t.tx_errors)
        .rate("drop", net_tx_drop_, t.tx_dropped)
        .rate("enqf", net_tx_enqf_, t.tx_enqueue_failures)
        .field("q", t.tx_queue_depth)
        .rate("hb_recv", net_hb_recv_, s.net.heartbeat_received)
        .rate("hb_rej", net_hb_rej_, s.net.heartbeat_rejected)
        .rate("sess_est", net_sess_est_, s.net.sessions_established)
        .rate("sess_ref", net_sess_ref_, s.net.sessions_refreshed)
        .rate("hb_ack", net_hb_ack_, s.net.heartbeat_ack_attempts)
        .rate("mal", net_mal_, s.net.malformed_datagrams)
        .rate("nonhb", net_nonhb_, s.net.non_heartbeat_datagrams);
    // 两族都在服务时才逐族展开：此时"零 rx"能定位到具体一族（v6 客户端被防火墙挡掉
    // 这类问题一眼可见）；单族实例的合计就等于那一族，展开只是噪声。
    if (pf.served_v4 && pf.served_v6) {
        b.rate("v4rx", net_v4_rx_, pf.v4.rx_packets)
            .rate("v4tx", net_v4_tx_, pf.v4.tx_packets)
            .rate("v4hb", net_v4_hb_, s.net.heartbeat_received_v4)
            .rate("v6rx", net_v6_rx_, pf.v6.rx_packets)
            .rate("v6tx", net_v6_tx_, pf.v6.tx_packets)
            .rate("v6hb", net_v6_hb_, s.net.heartbeat_received_v6);
    }
    return b.str();
}

std::string ServerSnapshotView::render_sessions(const ServerDiagnosticsSnapshot& s) const
{
    const auto& ss = s.session;
    FieldBlock b;
    b.field("act", ss.active)
        .rate("crt", sess_created_, ss.created)
        .rate("con", sess_connected_, ss.connected)
        .rate("ref", sess_refreshed_, ss.refreshed)
        .rate("rem", sess_removed_, ss.removed)
        .rate("exp", sess_expired_, ss.expired)
        .rate("rmbc", sess_rmbyclear_, ss.removed_by_clear)
        // 活跃度年龄：act 只说"有几个"，不说"它们多久没动"。oldest 大 = 那个
        // session 快被 reap 了（半死的连接在聚合计数里看不出来）。
        .field("age_n", ss.activity_count)
        .field("age_old", ss.activity_oldest_age_ms)
        .field("age_new", ss.activity_newest_age_ms)
        // 存活时长（距 created_at）：与 age_old（距上次控制面 keepalive，1Hz 下是
        // 锯齿状）是两个量——单看 age_old 容易误判成"会话在反复重建"。
        .field("sess_ms", ss.session_age_ms);
    return b.str();
}

// ============================================================
// ClientSnapshotView
// ============================================================

std::string ClientSnapshotView::render_state(const ClientDiagnosticsSnapshot& s) const
{
    FieldBlock b;
    b.field("state", std::string_view(runtime::runtime_state_name(s.state)))
        .field("route", audio::route_intent_owner_label(s.route_mode))
        .field("ls", std::format("{}/{}ms", audio::switch_outcome_name(s.switch_result.outcome), s.switch_result.duration_ms))
        .field("seq", s.switch_seq);
    return b.str();
}

std::string ClientSnapshotView::render_net(const ClientDiagnosticsSnapshot& s) const
{
    const auto& n = s.net;
    const auto& t = n.transport;
    FieldBlock b;
    b.rate("rx", net_rx_, t.rx_packets)
        .rate("rxB", net_rx_bytes_, t.rx_bytes)
        .rate("rxerr", net_rx_err_, t.rx_errors)
        .rate("rxunr", net_rx_unr_, t.rx_unreachable)
        .rate("tx", net_tx_, t.tx_packets)
        .rate("txB", net_tx_bytes_, t.tx_bytes)
        .rate("txerr", net_tx_err_, t.tx_errors)
        .rate("drop", net_tx_drop_, t.tx_dropped)
        .rate("enqf", net_tx_enqf_, t.tx_enqueue_failures)
        .field("q", t.tx_queue_depth)
        .rate("ack", net_hb_ack_, n.heartbeat_ack_count)
        .field("miss", n.heartbeat_ack_misses)
        .field("age", std::format("{}ms", n.heartbeat_ack_age_ms))
        .field("hb_fail", n.heartbeat_failed)
        .rate("hshk", net_hshk_, n.heartbeat_handshake_send_attempts)
        .rate("miss_ev", net_hb_miss_ev_, n.heartbeat_ack_miss_events)
        .rate("af", net_audio_, n.audio_frames_accepted)
        .rate("gap", net_gap_, n.rx_audio_sequence_gap_events)
        .rate("gap_fr", net_gap_frames_, n.rx_audio_sequence_missing_frames)
        .rate("mal", net_mal_, n.malformed_datagrams)
        .rate("unsnd", net_unexp_, n.unexpected_sender_datagrams)
        .rate("wsa", net_wrong_ack_, n.wrong_session_acks)
        .rate("plm", net_pl_mismatch_, n.audio_payload_mismatches)
        .rate("nona", net_non_audio_, n.non_audio_datagrams)
        .field("jit", n.estimator_jitter_ms, 2)
        .field("base", n.estimator_base_delay_ms, 2)
        .field("tr", n.estimator_transit_ms, 2)
        .field("trlvl", n.estimator_transit_level_ms, 1)
        .field("tstep", n.estimator_transit_step_events)
        .field("reord", n.estimator_reordered_packets)
        .field("dup", n.estimator_duplicate_packets)
        .field("late", n.estimator_late_packets)
        // 第二轮诊断扩充（2026-10-19）：
        // · 观测分母：est_pk 是丢包率的分母（此前只有分子）
        // · base_set：消掉 "base_delay=0 是没建立还是真为 0" 的歧义
        // · rx_consec：接收路径正在退避（>0 时 5→250ms 退避生效，是直接的延迟项）
        // · rx_ecode：原始错误码——"对端发包超大 / 本地收缓冲溢出 / socket 真死"
        //   三种处置完全不同，混在一个 rxerr 里分不出来
        // · learned：实际对端（观测值），可能与通告地址不同
        .rate("est_pk", net_est_pk_, s.net_ex.estimator_packets)
        .rate("est_gap", net_est_gap_, s.net_ex.estimator_gap_events)
        .rate("est_miss", net_est_miss_, s.net_ex.estimator_missing_packets)
        .field("base_set", s.net_ex.estimator_base_set)
        .field("rx_consec", s.net_ex.rx_consecutive_errors)
        .field("rx_ecode", s.net_ex.rx_last_error_code)
        .field("udp_fam", s.net_ex.udp_family)
        .field("sentinel", s.net_ex.udp_address_from_sentinel)
        .field("learned", std::string_view(s.net_ex.learned_peer_endpoint));
    return b.str();
}

std::string ClientSnapshotView::render_jb(const ClientDiagnosticsSnapshot& s) const
{
    const auto& jb = s.jitter_buffer;
    FieldBlock b;
    b.field("water", jb.water_level, 2)
        .field("used", std::format("{}/{}", jb.used_slots, jb.capacity_slots))
        .field("lead", std::format("{}({:.1f}ms)", jb.lead_slots, jb.lead_ms))
        .field("tgt", std::format("{}({:.1f}ms)", jb.target_slots, jb.target_ms))
        .field("play", jb.play_sequence)
        .field("high", jb.highest_received_sequence)
        .rate("reanc", jb_reanchor_, jb.reanchor_count)
        .rate("req", jb_reanchor_req_, jb.reanchor_requests)
        .rate("canc", jb_reanchor_cancel_, jb.reanchor_cancels)
        .rate("san", jb_reanchor_sanity_, jb.reanchor_sanity_rejections)
        .field("pend", jb.reanchor_pending)
        .field("tgtseq", jb.reanchor_target_sequence)
        .field("csil", jb.consecutive_silence_frames)
        .field("msil", jb.max_silence_run_frames)
        .field("ep", std::string_view(jb_episode(jb.episode_state)))
        .rate("push_ok", jb_push_ok_, jb.push_accepted)
        .rate("push_rej", jb_push_rej_, jb.push_rejected)
        .rate("late", jb_late_, jb.push_rejected_late)
        .rate("busy", jb_busy_, jb.push_rejected_slot_busy)
        .rate("inv", jb_invalid_, jb.push_rejected_invalid)
        .rate("sanity", jb_sanity_, jb.push_rejected_sanity)
        .rate("pull", jb_pull_, jb.pull_calls)
        .rate("pf", jb_pull_frames_, jb.pull_frames)
        .rate("sil", jb_silence_, jb.pull_silence_frames)
        .rate("fill_ep", jb_fill_ep_, jb.fill_episodes)
        .rate("fill_sl", jb_fill_sl_, jb.fill_corrected_slots)
        .rate("drop_ep", jb_drop_ep_, jb.drop_episodes)
        .rate("skip", jb_skip_, jb.drop_skipped_slots)
        .rate("splice", jb_splice_, jb.splice_events)
        .rate("und_ev", jb_und_ev_, jb.underrun_events)
        .rate("und_fr", jb_und_fr_, jb.underrun_frames)
        .field("uratio", jb.underrun_ratio, 6)
        .field("max_und", jb.max_consecutive_underrun_slots)
        .rate("conceal", jb_conceal_, jb.concealed_slots)
        .rate("csat", jb_conceal_sat_, jb.concealed_saturated_slots)
        .rate("fu", jb_fu_, jb.late_useful_packets)
        .field("fduty", jb.fill_duty, 6)
        .field("dduty", jb.drop_duty, 6)
        // 第二轮诊断扩充（2026-10-19）：
        // · oldest：第三项序列。没有它分不清 "lead=40 是 40 槽真音频" 还是
        //   "40 槽里全是洞"（lead/highest 都基于播放头，occupied span 才是真实占用）
        // · conceal_on / splice_on：开关可见性——concealed=0 时分不清"没触发"
        //   还是"掩盖功能根本没开"
        .field("oldest", s.jitter_buffer_ex.oldest_received_sequence)
        .field("conceal_on", s.jitter_buffer_ex.conceal_enabled)
        .field("conceal_max", s.jitter_buffer_ex.conceal_max_slots)
        .field("splice_on", s.jitter_buffer_ex.splice_enabled)
        .field("splice_xf", s.jitter_buffer_ex.splice_xfade_frames)
        .field("startup", s.jitter_buffer_ex.startup_slots)
        .field("init_tgt", s.jitter_buffer_ex.initial_target_slots);
    return b.str();
}

std::string ClientSnapshotView::render_jc(const ClientDiagnosticsSnapshot& s) const
{
    const auto& jc = s.jitter_control;
    const auto& jb = s.jitter_buffer;
    const double packet_ms = jb.target_slots != 0 ? jb.target_ms / static_cast<double>(jb.target_slots) : 0.0;
    FieldBlock b;
    b.field("adaptive", jc.adaptive)
        .field("desired", jc.desired_slots)
        .field("tailm", jc.tail_margin_slots, 2)
        .field("p99", jc.tail_p99_ms, 1)
        .field("tsamp", jc.tail_samples)
        .field("min", std::format("{}({:.1f}ms)", jc.min_slots, static_cast<double>(jc.min_slots) * packet_ms))
        .field("max", jc.max_slots)
        .field("geo", std::format("{}({:.1f}ms)", jc.geometric_floor_slots, static_cast<double>(jc.geometric_floor_slots) * packet_ms))
        // string_view 显式包裹：const char* 会因模板约束（仅算术/枚举）掉进
        // bool 重载，把名字打成 true（实测 src=/path= 恒为 true）。
        .field("src", std::string_view(audio::target_margin_source_name(static_cast<audio::TargetMarginSource>(jc.margin_source))))
        .field("path", std::string_view(audio::target_path_name(static_cast<audio::TargetPath>(jc.path))))
        .field("fl_b", jc.floor_bound)
        .field("cap_b", jc.cap_bound)
        .field("pen", jc.underrun_penalty, 2)
        .field("dwell", jc.dwell_remaining_ms, 0)
        .field("fall", jc.fall_room_slots, 2)
        .field("stall", jc.stall_events)
        .field("peak", jc.stall_peak_ms, 1)
        .field("lastgap", jc.last_stall_gap_ms, 1)
        .field("arr", jc.arrival_interval_ms, 3)
        .field("bw_lo", jc.band_warning_low)
        .field("bn_lo", jc.band_normal_low)
        .field("bn_hi", jc.band_normal_high)
        .field("bw_hi", jc.band_warning_high)
        .field("crun", jc.conceal_run_slots)
        .field("urun", jc.underrun_run_slots)
        // 第二轮诊断扩充（2026-10-19）：拆解"target 为什么是这个值"���最后几项。
        // · eff_min：真正生效的下限 = min_target + trunc(underrun_penalty)。
        //   没有它，"target=7" 无法拆解成"配置写 3 + 欠载惩罚 4"
        // · in_storm：风暴保持态（逐拍量，1Hz 采样抓不到瞬态，path=6 撞不上）
        // · stall_m：峰值项本轮给了几槽（margin_source 只说谁赢，输的那项不可见）
        // · fall_rate：跌侧限速速率（解释"这一拍为什么只降一格"）
        .field("eff_min", s.jitter_control_ex.effective_min_slots)
        .field("storm", s.jitter_control_ex.in_storm)
        .field("stall_m", s.jitter_control_ex.last_stall_margin_slots, 2)
        .field("stall_cap", s.jitter_control_ex.stall_peak_cap_slots, 2)
        .field("fall_rate", s.jitter_control_ex.fall_rate_slots_per_sec, 2);
    return b.str();
}

std::string ClientSnapshotView::render_playback(const ClientDiagnosticsSnapshot& s,
    std::string_view last_audio_error_name) const
{
    FieldBlock b;
    b.field("running", s.playback_running)
        .field("pstate", std::string_view(audio::playback_state_name(s.playback_state)))
        .field("err", last_audio_error_name)
        .rate("pull", pb_pull_, s.playback.pull_calls)
        .rate("pf", pb_frames_, s.playback.pull_frames)
        .rate("sil", pb_silence_, s.playback.pull_silence_frames)
        // 第二轮诊断扩充（2026-10-19）：
        // · stale：事件归属契约的"唯一可观测量"（文档钦定，此前两个快照里都没有）
        // · empty_skip：WASAPI 侧唯一的 underrun 信号（AAudio 有 xRun，Windows 没有）
        // · catchup：上次事件时可写帧数，远大于引擎周期即"迟到了多少帧"
        // · cb_frames：几何地板的权威输入（stream 的回退值只是启动期近似）
        .rate("stale", pb_stale_, s.playback_ex.stale_events_dropped)
        .rate("empty_skip", pb_empty_skip_, s.playback_ex.empty_event_skips)
        .field("catchup", s.playback_ex.catchup_frames)
        .field("catchup_max", s.playback_ex.catchup_frames_max)
        .field("cb_frames", s.playback_ex.callback_frames)
        .field("rate_adj", s.playback_ex.sample_rate_adjusted);
    return b.str();
}

std::string ClientSnapshotView::render_stream(const ClientDiagnosticsSnapshot& s) const
{
    const auto& st = s.stream;
    FieldBlock b;
    b.field("backend", audio::audio_stream_backend_name(st.backend))
        .field("rate", st.sample_rate)
        .field("ch", st.channels)
        .field("perf", audio::audio_stream_performance_name(st.performance_mode))
        .field("fpb", st.frames_per_burst)
        .field("cap", st.buffer_capacity_frames)
        .field("cb", st.callback_count)
        .field("pad", st.current_padding_frames)
        .rate("xrun", stream_xrun_, st.xrun_count)
        // 第二轮诊断扩充（2026-10-19）：
        // · ep_*：引擎周期四界。fpb 现在报的是**选定周期**（历史报 fundamental，
        //   与真正传给 InitializeSharedAudioStream 的周期可能差数倍——几何地板
        //   因此算错，2026-10-19 已修）。ep_fund 是它的"粒度单位"（选定周期必是
        //   它的整数倍），两个都报才解释得了"周期为什么是 336 而最小是 48"
        // · resample：端点混音格式 ≠ 请求格式 ⇒ OS SRC 在线路里（Windows 上
        //   音频发飘/发晚的头号原因）
        // · mix_*：端点混音格式
        .field("ep", st.engine_period_frames)
        .field("ep_min", st.engine_period_min_frames)
        .field("ep_max", st.engine_period_max_frames)
        .field("ep_def", st.engine_period_default_frames)
        .field("ep_fund", st.engine_period_fundamental_frames)
        .field("resample", st.engine_resampling)
        .field("mix_rate", st.mix_sample_rate)
        .field("mix_ch", st.mix_channels)
        // 输出侧延迟：buffer_size = 后端实际缓冲（AAudio 决定输出延迟的是它，
        // 不是 capacity）；out_buf = 此刻压在输出设备缓冲里的帧数。
        .field("buf_size", st.buffer_size_frames)
        .field("out_buf", st.output_buffering_frames)
        // 远端协商格式（与本机播放流的实际参数对照）
        .field("rmt_rate", s.remote.sample_rate)
        .field("rmt_ch", s.remote.channels)
        .field("rmt_enc", s.remote.encoding)
        .field("rmt_kbps", s.remote.bitrate_kbps)
        .field("rmt_F", s.remote.frame_count);
    return b.str();
}

} // namespace aqua::diagnostics
