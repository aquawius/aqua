#ifndef AQUA_DIAGNOSTICS_SNAPSHOT_VIEW_H
#define AQUA_DIAGNOSTICS_SNAPSHOT_VIEW_H

// 诊断快照渲染视图：把一份快照（ClientDiagnosticsSnapshot /
// ServerDiagnosticsSnapshot）渲染成若干个紧凑块，每个块一个模块
// （client 侧 state/net/jb/jc/pb/stream，server 侧 state/audio/capture/pktz/queue/dsp/net/sess）。
// 本文件产出块的**内部内容**，外层 `module{...}` 由 SnapshotLine 加。
//
// **CLI-only**：与 SnapshotLine 一样只服务 CLI 日志；C API / JNI / Android 只消费
// 快照本身（无时间状态的 POD 契约），不经过本文件。
//
// 每个模块拥有一组**持久** RateCounter——这就是该模块的"诊断 State 结构"
// （类比 udp_server.h / udp_client.h 把运行态收敛进各自 State）。跨拍的
// delta/rate 状态只活在 SnapshotView 实例里，不参与快照（快照仍是纯值语义 POD，
// 供 C API / Android 共用），两者解耦：快照负责"采什么"，SnapshotView 负责"怎么显示"。
//
// 读法见 aqua_core/doc/diagnostics.md。

#include "aqua/audio/capture/audio_capture_config.h"
#include "aqua/diagnostics/client_diagnostics_snapshot.h"
#include "aqua/diagnostics/field_block.h"
#include "aqua/diagnostics/server_diagnostics_snapshot.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aqua::diagnostics {

// ---- Server 侧：audio / capture / pktz / queue / dsp / net / sess ----
class ServerSnapshotView {
public:
    explicit ServerSnapshotView(audio::AudioCaptureSource capture_source);

    // 每个方法渲染一个模块块的内部文本（SnapshotLine 负责包成 module{...}）。
    std::string render_state(const ServerDiagnosticsSnapshot& s, std::uint16_t udp_port) const;
    std::string render_audio(const ServerDiagnosticsSnapshot& s) const;
    std::string render_capture(const ServerDiagnosticsSnapshot& s) const;
    std::string render_packetizer(const ServerDiagnosticsSnapshot& s) const;
    std::string render_queue(const ServerDiagnosticsSnapshot& s) const;
    std::string render_dispatcher(const ServerDiagnosticsSnapshot& s) const;
    std::string render_net(const ServerDiagnosticsSnapshot& s) const;
    std::string render_sessions(const ServerDiagnosticsSnapshot& s) const;

private:
    audio::AudioCaptureSource capture_source_;

    // capture 模块速率计数器
    mutable RateCounter cap_audio_events_, cap_packet_queries_, cap_packet_empty_;
    mutable RateCounter cap_packets_ready_, cap_get_buffer_, cap_callbacks_;
    mutable RateCounter cap_silent_callbacks_, cap_synth_silence_, cap_gen_silence_frames_;
    mutable RateCounter cap_starved_events_, cap_starved_ms_, cap_captured_frames_;
    mutable RateCounter cap_captured_bytes_;
    // packetizer 模块
    mutable RateCounter pktz_blocks_, pktz_bytes_, pktz_frames_, pktz_unaligned_, pktz_discards_;
    // queue 模块
    mutable RateCounter queue_accepted_, queue_consumed_, queue_dropped_;
    // dispatcher 模块
    mutable RateCounter dsp_encoded_, dsp_broadcast_, dsp_no_clients_, dsp_encode_fail_;
    mutable RateCounter dsp_dispatch_fail_, dsp_dropped_, dsp_published_, dsp_wakeups_;
    // net 模块
    mutable RateCounter net_rx_, net_rx_bytes_, net_rx_err_, net_rx_unr_, net_tx_, net_tx_bytes_;
    mutable RateCounter net_tx_err_, net_tx_drop_, net_tx_enqf_, net_hb_recv_, net_hb_rej_;
    mutable RateCounter net_sess_est_, net_sess_ref_, net_hb_ack_, net_mal_, net_nonhb_;
    // session 模块
    mutable RateCounter sess_created_, sess_connected_, sess_refreshed_, sess_removed_;
    mutable RateCounter sess_expired_, sess_rmbyclear_;
};

// ---- Client 侧：state / net / jb / jc / pb / stream ----
class ClientSnapshotView {
public:
    // 每个方法渲染一个模块块的内部文本。
    std::string render_state(const ClientDiagnosticsSnapshot& s) const;
    std::string render_net(const ClientDiagnosticsSnapshot& s) const;
    std::string render_jb(const ClientDiagnosticsSnapshot& s) const;
    std::string render_jc(const ClientDiagnosticsSnapshot& s) const;
    std::string render_playback(const ClientDiagnosticsSnapshot& s,
        std::string_view last_audio_error_name) const;
    std::string render_stream(const ClientDiagnosticsSnapshot& s) const;

private:
    // net 模块
    mutable RateCounter net_rx_, net_rx_bytes_, net_rx_err_, net_rx_unr_, net_tx_, net_tx_bytes_;
    mutable RateCounter net_tx_err_, net_tx_drop_, net_tx_enqf_, net_hb_ack_, net_hb_miss_ev_;
    mutable RateCounter net_hshk_, net_audio_, net_gap_, net_gap_frames_, net_mal_, net_unexp_;
    mutable RateCounter net_wrong_ack_, net_pl_mismatch_, net_non_audio_;
    // jb 模块
    mutable RateCounter jb_push_ok_, jb_push_rej_, jb_late_, jb_busy_, jb_invalid_, jb_sanity_;
    mutable RateCounter jb_pull_, jb_pull_frames_, jb_silence_, jb_fill_ep_, jb_fill_sl_;
    mutable RateCounter jb_drop_ep_, jb_skip_, jb_splice_, jb_und_ev_, jb_und_fr_, jb_conceal_, jb_conceal_sat_;
    mutable RateCounter jb_fu_, jb_reanchor_, jb_reanchor_req_, jb_reanchor_cancel_, jb_reanchor_sanity_;
    // playback 模块
    mutable RateCounter pb_pull_, pb_frames_, pb_silence_, stream_xrun_;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_SNAPSHOT_VIEW_H
