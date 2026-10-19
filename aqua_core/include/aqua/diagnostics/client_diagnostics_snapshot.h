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
#include "aqua/audio/devices/route_policy.h"
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
    // 意图归属（RoutePolicy 的诊断投影；取值即 aqua_route_mode 编码）。
    audio::RouteIntentOwner route_mode = audio::RouteIntentOwner::None;
    // 最近一次切换事务的结果（None = 尚未发生切换）。
    audio::SwitchResult switch_result { };
    // 请求设备（User 归属时有值；空 = 无显式请求）。
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
        double estimator_transit_level_ms = 0.0; // transit 慢跟随电平（EWMA，纯诊断）
        std::uint64_t estimator_transit_step_events = 0; // 单包 |Δtransit| 超阈值的次数
        double estimator_last_transit_step_ms = 0.0; // 最近一次台阶的带符号幅度
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
        std::uint64_t splice_events = 0; // crossfade arm 次数（splice 关时恒 0）

        // ---- Phase 2 欠载预算 + PCM concealment ----
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
        double lead_ms = 0.0; // lead 换算毫秒（与 target_ms 同口径；lead_slots+lead_ms 同快照）
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
    // （其后已追加 switch_seq：它只依赖 playback_，故放在本组之外。
    //   再追加字段请排在 switch_seq 之后，保持「末尾追加」这一 ABI 约定。）
    //
    // 三部分来源不同，故也不塞进上面的单个 `if (jb_)`/`if (estimator_)` 块：
    //   controller（决策层） / estimator（观测层尾部） / jb（执行层带与 run）。
    struct JitterControl {
        // ---- 决策层：TargetController（仅自适应模式；adaptive=false 时全 0）----
        bool adaptive = false; // target 是否由 TargetController 输出（false = --jb-fixed-target 固定模式）
        std::uint32_t desired_slots = 0; // 未限速期望值；与 target_slots 不等 = 被限速/dwell/风暴冻结按住
        std::uint32_t min_slots = 0; // 生效下限 = max(--jb-min-target, 几何地板 + 1)
        std::uint32_t max_slots = 0; // 结构上限 = 2/3 × capacity
        std::int32_t margin_source = 0; // margin 胜出方：0=tail_p99 1=stall_peak
        std::int32_t path = 0; // 本拍收敛路径：0=steady 1=rise 2=fall 3=dwell_lock 4=deadband
            // 5=no_time_base 6=storm_hold（4=deadband 已废弃：死区恒 0，ADR-4，永不可达）
        bool floor_bound = false; // desired 被下限抬起（margin 失算，安全网托住）
        bool cap_bound = false; // desired 被结构上限夹住（正在兜底）
        double underrun_penalty = 0.0; // 欠载反馈抬升量（槽）；>0 = 闭环在工作（预期，非故障）
        double dwell_remaining_ms = 0.0; // 涨后锁跌剩余（ms）；>0 = 正在锁跌（峰值保持）
        double fall_room_slots = 0.0; // 本拍跌侧限速额度（槽）；解释"这一拍为什么只降一格"
        // 几何地板（槽）= 一次 playback callback 消耗的包数（ceil(callback_frames/F)），
        // 生效下限还 +1 并与 --jb-min-target 取大。单独暴露它才能回答"现在的最低延迟是
        // 网络需要的，还是 playback callback 几何逼出来的"（min_slots 只是合成结果）。
        std::uint32_t geometric_floor_slots = 0;

        // ---- 尾部诊断（抖动项 P99 的深度读数）----
        // 快照 1/s 采样：回答"P99 成熟了吗（tsamples）"、"尾部在哪
        // （tail_p99_ms）"、"抖动项给了几槽（tail_margin_slots）"。
        // 逐包轨迹（门限跨越时刻、冷启动填充过程）看 --jb-trace 的 JBT 行。
        double tail_p99_ms = 0.0; // 尾部直方图 P99（ms，单包绝对偏差）；<0 = 窗口未满
        std::uint64_t tail_samples = 0; // 窗内有效样本数（满窗 = JB_TAIL_WINDOW_PACKETS；< JB_TAIL_MIN_SAMPLES 时 P99 无效）
        double tail_margin_slots = 0.0; // 抖动项 = P99/包周期 + 相位余量（槽，取 max 之前）

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

    // ---- 播放设备切换事务序号（末尾追加）----
    // 每笔切换事务（成功 / 回滚 / 兜底 / 链耗尽）递增一次。outcome+error 只能
    // 表达"最近一次结果"，两笔不同事务可能完全相同（都是 Switched+None），
    // UI 需要序号才能判定"又切了一次"。设备 id 在部分平台回读为空，不能当
    // 唯一判据。来源：PlaybackManager::switch_seq()。
    std::uint32_t switch_seq = 0;

    // =====================================================================
    // 第二轮诊断扩充（2026-10-19；**全部末尾追加，排在 switch_seq 之后**——
    // C API 只能末尾追加才保 ABI，见 aqua_capi.h 的约定）
    //
    // 统一单位规则：**内部计量一律 frames / slots / ns**，任何 *_ms 只在
    // 快照边界按 F/sample_rate 派生（音频格式会变，ms↔帧的换算随之变化，
    // 拿 ms 做内部量会在换格式时静默失真）。
    // =====================================================================

    // ---- 输出流：引擎周期与格式协商（回答"这一拍到底多大、谁在重采样"）----
    struct StreamEx {
        std::uint32_t engine_period_frames = 0; // **选定**的引擎周期（WASAPI 传给
            // InitializeSharedAudioStream 的那个；frames_per_burst 历史上存的是
            // fundamental，两者在 requested==0 时可能差数倍——几何地板修正后
            // 以本字段为准）。AAudio = burst size。
        std::uint32_t engine_period_min_frames = 0; // 设备允许的最小周期（0 = 未上报）
        std::uint32_t engine_period_max_frames = 0; // 设备允许的最大周期（0 = 未上报）
        std::uint32_t engine_period_default_frames = 0; // 引擎默认周期（0 = 未上报）
        std::uint32_t engine_period_fundamental_frames = 0; // 引擎基本周期（选定周期的
            // "粒度单位"：选定周期必是它的整数倍，两个都报出来才解释得了
            // "周期为什么是 336 而最小周期是 48"）
        bool engine_resampling = false; // 端点混音格式的采样率/声道数与请求格式
            // 不同 ⇒ 引擎要做重采样/混音转换（OS SRC 在线路里——Windows 上音频
            // 发飘/发晚的头号原因）。纯位深/编码差异不置位（等精度转换，无害）。
        std::uint32_t mix_sample_rate = 0; // 端点混音格式采样率（0 = 未上报）
        std::uint32_t mix_channels = 0; // 端点混音格式声道数（0 = 未上报）
        // ---- 输出侧延迟（2026-10-19 补）----
        std::uint32_t buffer_size_frames = 0; // 后端实际缓冲帧数（AAudio = 真正
            // 决定输出延迟的 buffer size；WASAPI = 端点缓冲）。0 = 未上报
        std::uint32_t output_buffering_frames = 0; // **此刻**压在输出设备缓冲里的
            // 帧数（= 输出侧延迟地板；除以采样率即毫秒）。0 = 未上报
    } stream_ex;

    // ---- playback 消费侧补充（后端层计数 + RT 回调几何）----
    struct PlaybackEx {
        std::uint64_t stale_events_dropped = 0; // 播放事件因过期被丢弃的次数
            // （PlaybackManager 契约的"唯一可观测量"——文档钦定却从未暴露）
        std::uint64_t empty_event_skips = 0; // WASAPI：事件到达但端点缓冲已满
            // （available_frames==0）而跳过的次数 = **Windows 侧唯一的 underrun 信号**
            // （AAudio 有 xRun，WASAPI 原来一个都没有）
        std::uint32_t catchup_frames = 0; // 上一次事件时可写的帧数（≈ 迟到多少帧）
        std::uint32_t catchup_frames_max = 0; // 本次运行最大可写帧数
        std::uint32_t callback_frames = 0; // 最近一次设备回调请求的帧数
            // （几何地板的权威输入；stream.frames_per_burst 是启动期回退值）
        bool sample_rate_adjusted = false; // AAudio：系统 SRC 改过采样率（WASAPI 恒 false）
    } playback_ex;

    // ---- 网络：退避 / 错误分类 / 观测分母 / 对端身份 ----
    struct NetEx {
        std::uint32_t rx_consecutive_errors = 0; // 连续收包错误数（退避判定量；
            // >0 = 接收路径正在指数退避，直接是延迟贡献项）
        std::int32_t rx_last_error_code = 0; // 最近一次收包错误的原始错误码
            // （WSAEMSGSIZE / WSAENOBUFS / socket 死亡的处置完全不同）
        std::uint64_t estimator_packets = 0; // estimator 观测包总数（丢包率的分母）
        std::uint64_t estimator_gap_events = 0; // estimator 的 seq 缺口事件数
        std::uint64_t estimator_missing_packets = 0; // estimator 累计缺失包数
        bool estimator_base_set = false; // base_delay 是否已确立（0 = 未建立，不是真为 0）
        bool udp_address_from_sentinel = false; // 服务端通告了 0.0.0.0，数据面因此
            // 复用控制面地址——一整类"地址看起来不对"的自证
        std::int32_t udp_family = 0; // 数据面实际使用的族（0=未知 4=IPv4 6=IPv6）
        // 实际对端（观测值）：服务端**真正**回话的地址。它可能与通告地址不同
        // （NAT / 多网卡 / 通告的是通配地址），而通告地址与实际地址此前只出现在
        // aqua_connect_result_t 里，不在诊断快照里——"我到底在跟谁说话"这个问题
        // 在诊断侧无从回答。空 = 尚未学到（还没收到过对端回包）。
        std::string learned_peer_endpoint; // "ip:port"
    } net_ex;

    // ---- 抖动缓冲：第三项序列 + 开关可见性（0 = 没触发 还是 功能没开？）----
    struct JitterBufferEx {
        std::uint64_t oldest_received_sequence = 0; // 缓冲里最旧的已收序列
            // （lead/highest 都基于播放头；没有它分不清"40 槽真音频"和"40 槽里全是洞"）
        bool conceal_enabled = false; // repeat-last 掩盖是否开启
        std::uint32_t conceal_max_slots = 0; // 连续掩盖上限（槽）
        bool splice_enabled = false; // 拼接 crossfade 是否开启
        std::uint32_t splice_xfade_frames = 0; // crossfade 长度（采样帧）
        std::uint32_t startup_slots = 0; // 启动预填充槽数
        std::uint32_t initial_target_slots = 0; // 启动初始 target（槽）
    } jitter_buffer_ex;

    // ---- 决策层补充（拆解"target 为什么是这个值"的最后几项）----
    struct JitterControlEx {
        std::uint32_t effective_min_slots = 0; // 真正生效的下限 = min_target +
            // trunc(underrun_penalty)（没有它，"target=7"无法拆解）
        bool in_storm = false; // 当前处于风暴保持态（逐拍量，1Hz 采样抓不到瞬态）
        double last_stall_margin_slots = 0.0; // 峰值项本轮给了几槽
        double stall_peak_cap_slots = 0.0; // 峰值项上限（槽）
        double fall_rate_slots_per_sec = 0.0; // 跌侧限速速率（解释"这一拍为什么只降一格"）
    } jitter_control_ex;

    // ---- 远端协商格式（回答"服务端给的格式和我播的为什么不一样"）----
    struct RemoteFormat {
        std::uint32_t sample_rate = 0;
        std::uint32_t channels = 0;
        std::int32_t encoding = 0; // audio::PcmEncoding
        std::uint32_t bitrate_kbps = 0;
        std::uint32_t frame_count = 0; // 每包采样帧数 F（槽→ms 换算的另一个输入）
    } remote;
};

} // namespace aqua::diagnostics

#endif // AQUA_DIAGNOSTICS_CLIENT_DIAGNOSTICS_SNAPSHOT_H
